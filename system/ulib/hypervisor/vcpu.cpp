// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>
#include <string.h>

#include <fbl/unique_ptr.h>
#include <hw/pci.h>
#include <hypervisor/address.h>
#include <hypervisor/bits.h>
#include <hypervisor/block.h>
#include <hypervisor/decode.h>
#include <hypervisor/io_port.h>
#include <hypervisor/pci.h>
#include <hypervisor/vcpu.h>
#include <zircon/assert.h>
#include <zircon/syscalls.h>

#include "acpi_priv.h"

/* Interrupt vectors. */
#define X86_INT_GP_FAULT 13u

static zx_status_t unhandled_mem(const zx_packet_guest_mem_t* mem, const instruction_t* inst) {
    fprintf(stderr, "Unhandled address %#lx\n", mem->addr);
    if (inst->type == INST_MOV_READ)
        *inst->reg = UINT64_MAX;
    return ZX_OK;
}

static zx_status_t handle_mmio_read(vcpu_ctx_t* vcpu_ctx, uint64_t trap_key, zx_vaddr_t addr,
                                    uint8_t access_size, zx_vcpu_io_t* io) {
    switch (addr) {
    case PCI_ECAM_PHYS_BASE ... PCI_ECAM_PHYS_TOP:
        return vcpu_ctx->guest->pci_bus->ReadEcam(addr, access_size, io);
    }

    IoMapping& mapping = trap_key_to_mapping(trap_key);
    IoValue value = {};
    value.access_size = io->access_size;
    zx_status_t status = mapping.Read(addr, &value);
    if (status != ZX_OK)
        return status;

    io->access_size = value.access_size;
    io->u32 = value.u32;
    return ZX_OK;
}

static zx_status_t handle_mmio_write(vcpu_ctx_t* vcpu_ctx, uint64_t trap_key, zx_vaddr_t addr,
                                     zx_vcpu_io_t* io) {
    switch (addr) {
    case PCI_ECAM_PHYS_BASE ... PCI_ECAM_PHYS_TOP:
        return vcpu_ctx->guest->pci_bus->WriteEcam(addr, io);
    }

    IoMapping& mapping = trap_key_to_mapping(trap_key);
    IoValue value;
    value.access_size = io->access_size;
    value.u32 = io->u32;
    return mapping.Write(addr, value);
}

static zx_status_t handle_mmio(vcpu_ctx_t* vcpu_ctx, const zx_packet_guest_mem_t* mem,
                               uint64_t trap_key, const instruction_t* inst) {
    zx_status_t status;
    zx_vcpu_io_t mmio;
    if (inst->type == INST_MOV_WRITE) {
        switch (inst->mem) {
        case 1:
            status = inst_write8(inst, &mmio.u8);
            break;
        case 2:
            status = inst_write16(inst, &mmio.u16);
            break;
        case 4:
            status = inst_write32(inst, &mmio.u32);
            break;
        default:
            return ZX_ERR_NOT_SUPPORTED;
        }
        if (status != ZX_OK)
            return status;
        mmio.access_size = inst->mem;
        return handle_mmio_write(vcpu_ctx, trap_key, mem->addr, &mmio);
    }

    if (inst->type == INST_MOV_READ) {
        status = handle_mmio_read(vcpu_ctx, trap_key, mem->addr, inst->mem, &mmio);
        if (status != ZX_OK)
            return status;
        switch (inst->mem) {
        case 1:
            return inst_read8(inst, mmio.u8);
        case 2:
            return inst_read16(inst, mmio.u16);
        case 4:
            return inst_read32(inst, mmio.u32);
        default:
            return ZX_ERR_NOT_SUPPORTED;
        }
    }

    if (inst->type == INST_TEST) {
        status = handle_mmio_read(vcpu_ctx, trap_key, mem->addr, inst->mem, &mmio);
        if (status != ZX_OK)
            return status;
        switch (inst->mem) {
        case 1:
            return inst_test8(inst, static_cast<uint8_t>(inst->imm), mmio.u8);
        default:
            return ZX_ERR_NOT_SUPPORTED;
        }
    }

    return ZX_ERR_INVALID_ARGS;
}

static zx_status_t handle_mem(vcpu_ctx_t* vcpu_ctx, const zx_packet_guest_mem_t* mem,
                              uint64_t trap_key) {
    zx_vcpu_state_t vcpu_state;
    zx_status_t status = vcpu_ctx->read_state(vcpu_ctx, ZX_VCPU_STATE, &vcpu_state,
                                              sizeof(vcpu_state));
    if (status != ZX_OK)
        return status;

    instruction_t inst;
#if __x86_64__
    status = inst_decode(mem->inst_buf, mem->inst_len, &vcpu_state, &inst);
#else
    status = ZX_ERR_NOT_SUPPORTED;
#endif

    if (status != ZX_OK) {
        fprintf(stderr, "Unsupported instruction:");
#if __x86_64__
        for (uint8_t i = 0; i < mem->inst_len; i++)
            fprintf(stderr, " %x", mem->inst_buf[i]);
#endif // __x86_64__
        fprintf(stderr, "\n");
    } else {
        switch (mem->addr) {
        case LOCAL_APIC_PHYS_BASE ... LOCAL_APIC_PHYS_TOP:
            status = vcpu_ctx->local_apic.Handler(mem, &inst);
            break;
        default: {
            status = handle_mmio(vcpu_ctx, mem, trap_key, &inst);
            if (status == ZX_ERR_NOT_FOUND)
                status = unhandled_mem(mem, &inst);
            break;
        }
        }
    }

    if (status != ZX_OK) {
        return zx_vcpu_interrupt(vcpu_ctx->vcpu, X86_INT_GP_FAULT);
    } else if (inst.type == INST_MOV_READ || inst.type == INST_TEST) {
        // If there was an attempt to read or test memory, update the GPRs.
        return vcpu_ctx->write_state(vcpu_ctx, ZX_VCPU_STATE, &vcpu_state, sizeof(vcpu_state));
    }
    return status;
}

static zx_status_t handle_input(vcpu_ctx_t* vcpu_ctx, const zx_packet_guest_io_t* io,
                                uint64_t key) {
#if __x86_64__
    zx_status_t status = ZX_OK;
    zx_vcpu_io_t vcpu_io;
    memset(&vcpu_io, 0, sizeof(vcpu_io));
    switch (io->port) {
    case PCI_CONFIG_ADDRESS_PORT_BASE ... PCI_CONFIG_ADDRESS_PORT_TOP:
    case PCI_CONFIG_DATA_PORT_BASE ... PCI_CONFIG_DATA_PORT_TOP:
        status = vcpu_ctx->guest->pci_bus->ReadIoPort(io->port, io->access_size, &vcpu_io);
        break;
    default: {
        IoMapping& mapping = trap_key_to_mapping(key);
        IoValue value = {};
        value.access_size = io->access_size;
        status = mapping.Read(io->port, &value);
        vcpu_io.access_size = value.access_size;
        vcpu_io.u32 = value.u32;
    }
    }
    if (status != ZX_OK) {
        fprintf(stderr, "Unhandled port in %#x: %d\n", io->port, status);
        return status;
    }
    if (vcpu_io.access_size != io->access_size) {
        fprintf(stderr, "Unexpected size (%u != %u) for port in %#x\n", vcpu_io.access_size,
                io->access_size, io->port);
        return ZX_ERR_IO_DATA_INTEGRITY;
    }
    return vcpu_ctx->write_state(vcpu_ctx, ZX_VCPU_IO, &vcpu_io, sizeof(vcpu_io));
#else  // __x86_64__
    return ZX_ERR_NOT_SUPPORTED;
#endif // __x86_64__
}

static zx_status_t handle_output(vcpu_ctx_t* vcpu_ctx, const zx_packet_guest_io_t* io,
                                 uint64_t key) {
#if __x86_64__
    switch (io->port) {
    case PCI_CONFIG_ADDRESS_PORT_BASE ... PCI_CONFIG_ADDRESS_PORT_TOP:
    case PCI_CONFIG_DATA_PORT_BASE ... PCI_CONFIG_DATA_PORT_TOP:
        return vcpu_ctx->guest->pci_bus->WriteIoPort(io);
    default: {
        IoMapping& mapping = trap_key_to_mapping(key);
        IoValue value;
        value.access_size = io->access_size;
        value.u32 = io->u32;
        return mapping.Write(io->port, value);
    }
    }
    fprintf(stderr, "Unhandled port out %#x\n", io->port);
    return ZX_ERR_NOT_SUPPORTED;
#else  // __x86_64__
    return ZX_ERR_NOT_SUPPORTED;
#endif // __x86_64__
}

static zx_status_t handle_io(vcpu_ctx_t* vcpu_ctx, const zx_packet_guest_io_t* io, uint64_t key) {
    return io->input ? handle_input(vcpu_ctx, io, key) : handle_output(vcpu_ctx, io, key);
}

static zx_status_t vcpu_state_read(vcpu_ctx_t* vcpu_ctx, uint32_t kind, void* buffer,
                                   uint32_t len) {
    return zx_vcpu_read_state(vcpu_ctx->vcpu, kind, buffer, len);
}

static zx_status_t vcpu_state_write(vcpu_ctx_t* vcpu_ctx, uint32_t kind, const void* buffer,
                                    uint32_t len) {
    return zx_vcpu_write_state(vcpu_ctx->vcpu, kind, buffer, len);
}

vcpu_ctx::vcpu_ctx(zx_handle_t vcpu_, uintptr_t apic_addr_)
    :  vcpu(vcpu_), read_state(&vcpu_state_read), write_state(vcpu_state_write),
       local_apic(vcpu_, apic_addr_) {}

zx_status_t vcpu_loop(vcpu_ctx_t* vcpu_ctx) {
    while (true) {
        zx_port_packet_t packet;
        zx_status_t status = zx_vcpu_resume(vcpu_ctx->vcpu, &packet);
        if (status != ZX_OK) {
            fprintf(stderr, "Failed to resume VCPU %d\n", status);
            return status;
        }
        status = vcpu_packet_handler(vcpu_ctx, &packet);
        if (status != ZX_OK) {
            fprintf(stderr, "Failed to handle guest packet %d: %d\n", packet.type, status);
            return status;
        }
    }
}

zx_status_t vcpu_packet_handler(vcpu_ctx_t* vcpu_ctx, zx_port_packet_t* packet) {
    switch (packet->type) {
    case ZX_PKT_TYPE_GUEST_MEM:
        return handle_mem(vcpu_ctx, &packet->guest_mem, packet->key);
    case ZX_PKT_TYPE_GUEST_IO:
        return handle_io(vcpu_ctx, &packet->guest_io, packet->key);
    default:
        fprintf(stderr, "Unhandled guest packet %d\n", packet->type);
        return ZX_ERR_NOT_SUPPORTED;
    }
}

struct device_t {
    zx_handle_t port;
    device_handler_fn_t handler;
    void* ctx;

    device_t(zx_handle_t port, device_handler_fn_t handler, void* ctx)
        : port(port), handler(handler), ctx(ctx) {}
    ~device_t() { zx_handle_close(port); }
};

static int device_loop(void* ctx) {
    fbl::unique_ptr<device_t> device(static_cast<device_t*>(ctx));

    while (true) {
        zx_port_packet_t packet;
        zx_status_t status = zx_port_wait(device->port, ZX_TIME_INFINITE, &packet, 0);
        if (status != ZX_OK) {
            fprintf(stderr, "Failed to wait for device port %d\n", status);
            break;
        }

        status = device->handler(&packet, device->ctx);
        if (status != ZX_OK) {
            fprintf(stderr, "Unable to handle packet for device %d\n", status);
            break;
        }
    }

    return ZX_ERR_INTERNAL;
}

zx_status_t device_trap(zx_handle_t guest, const trap_args_t* traps, size_t num_traps,
                        device_handler_fn_t handler, void* ctx) {
    if (num_traps == 0)
        return ZX_ERR_INVALID_ARGS;

    // Only create a port if we have at least one BAR that requires it.
    bool create_port = false;
    for (size_t i = 0; !create_port && i < num_traps; ++i)
        create_port = create_port || traps[i].use_port;

    zx_handle_t port = ZX_HANDLE_INVALID;
    if (create_port) {
        zx_status_t status = zx_port_create(0, &port);
        if (status != ZX_OK) {
            fprintf(stderr, "Failed to create device port %d\n", status);
            return ZX_ERR_INTERNAL;
        }
    }

    auto device = fbl::make_unique<device_t>(port, handler, ctx);

    for (size_t i = 0; i < num_traps; ++i) {
        const trap_args_t* trap = &traps[i];
        zx_handle_t port = trap->use_port ? device->port : ZX_HANDLE_INVALID;
        zx_status_t status = zx_guest_set_trap(guest, trap->kind, trap->addr, trap->len, port,
                                               trap->key);
        if (status != ZX_OK)
            return ZX_ERR_INTERNAL;
    }

    if (create_port) {
        thrd_t thread;
        int ret = thrd_create(&thread, device_loop, device.release());
        if (ret != thrd_success) {
            fprintf(stderr, "Failed to create device thread %d\n", ret);
            return ZX_ERR_INTERNAL;
        }

        ret = thrd_detach(thread);
        if (ret != thrd_success) {
            fprintf(stderr, "Failed to detach device thread %d\n", ret);
            return ZX_ERR_INTERNAL;
        }
    }

    return ZX_OK;
}
