// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <err.h>
#include <platform.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <trace.h>

#include <kernel/auto_lock.h>
#include <kernel/vm/vm_object.h>
#include <kernel/vm/vm_region.h>

#include <mxtl/user_ptr.h>

#include <lib/console.h>
#include <lib/user_copy.h>
#include <lib/ktrace.h>

#include <lk/init.h>
#include <platform/debug.h>

#include <magenta/process_dispatcher.h>
#include <magenta/thread_dispatcher.h>
#include <magenta/user_copy.h>
#include <magenta/vm_object_dispatcher.h>

#include "syscalls_priv.h"

#define LOCAL_TRACE 0

constexpr uint32_t kMaxDebugWriteSize = 256u;
constexpr mx_size_t kMaxDebugReadBlock = 64 * 1024u * 1024u;

#if WITH_LIB_DEBUGLOG
#include <lib/debuglog.h>
#endif

int sys_debug_read(mx_handle_t handle, void* ptr, uint32_t len) {
    LTRACEF("ptr %p\n", ptr);

    // TODO: finer grained validation
    mx_status_t status;
    if ((status = validate_resource_handle(handle)) < 0) {
        return status;
    }

    if (!len)
        return 0;
    // TODO: remove this cast.
    auto uptr = reinterpret_cast<uint8_t*>(ptr);
    auto end = uptr + len;

    for (; uptr != end; ++uptr) {
        int c = getchar();
        if (c < 0)
            break;

        if (c == '\r')
            c = '\n';
        if (copy_to_user_u8_unsafe(uptr, static_cast<uint8_t>(c)) != NO_ERROR)
            break;
    }
    // TODO: fix this cast, which can overflow.
    return static_cast<int>(reinterpret_cast<char*>(uptr) - reinterpret_cast<char*>(ptr));
}

int sys_debug_write(const void* ptr, uint32_t len) {
    LTRACEF("ptr %p, len %d\n", ptr, len);

    if (len > kMaxDebugWriteSize)
        len = kMaxDebugWriteSize;

    char buf[kMaxDebugWriteSize];
    if (magenta_copy_from_user(ptr, buf, len) != NO_ERROR)
        return ERR_INVALID_ARGS;

    for (uint32_t i = 0; i < len; i++) {
        platform_dputc(buf[i]);
    }
    return len;
}

int sys_debug_send_command(mx_handle_t handle, const void* ptr, uint32_t len) {
    LTRACEF("ptr %p, len %d\n", ptr, len);

    // TODO: finer grained validation
    mx_status_t status;
    if ((status = validate_resource_handle(handle)) < 0) {
        return status;
    }

    if (len > kMaxDebugWriteSize)
        return ERR_INVALID_ARGS;

    char buf[kMaxDebugWriteSize + 2];
    if (magenta_copy_from_user(ptr, buf, len) != NO_ERROR)
        return ERR_INVALID_ARGS;

    buf[len] = '\n';
    buf[len + 1] = 0;
    return console_run_script(buf);
}

// Given a task (job or process), obtain a handle to that task's child
// which has the matching koid.  For now, a handle of MX_HANDLE_INVALID
// is the "root" handle that is the parent of processes.  This will
// change to a real root resource handle so only privileged callers may
// obtain top level processes (and eventually jobs) directly from a koid
mx_handle_t sys_debug_task_get_child(mx_handle_t handle, uint64_t koid) {
    const auto kDebugRights =
        MX_RIGHT_READ | MX_RIGHT_WRITE | MX_RIGHT_DUPLICATE | MX_RIGHT_TRANSFER;

    auto up = ProcessDispatcher::GetCurrent();

    if (handle == MX_HANDLE_INVALID) {
        //TODO: lookup process from job
        //TODO: lookup job from global resource handle
        // for now handle == 0 means look up process globally
        auto process = ProcessDispatcher::LookupProcessById(koid);
        if (!process)
            return ERR_NOT_FOUND;

        HandleUniquePtr process_h(
            MakeHandle(mxtl::RefPtr<Dispatcher>(process.get()), kDebugRights));
        if (!process_h)
            return ERR_NO_MEMORY;

        auto process_hv = up->MapHandleToValue(process_h.get());
        up->AddHandle(mxtl::move(process_h));
        return process_hv;
    }

    mxtl::RefPtr<Dispatcher> dispatcher;
    uint32_t rights;
    if (!up->GetDispatcher(handle, &dispatcher, &rights))
        return ERR_BAD_HANDLE;

    auto process = dispatcher->get_specific<ProcessDispatcher>();
    if (process) {
        auto thread = process->LookupThreadById(koid);
        if (!thread)
            return ERR_NOT_FOUND;
        auto td = mxtl::RefPtr<Dispatcher>(thread->dispatcher());
        if (!td)
            return ERR_NOT_FOUND;
        HandleUniquePtr thread_h(MakeHandle(td, kDebugRights));
        if (!thread_h)
            return ERR_NO_MEMORY;

        auto thread_hv = up->MapHandleToValue(thread_h.get());
        up->AddHandle(mxtl::move(thread_h));
        return thread_hv;
    }

    return ERR_WRONG_TYPE;
}

mx_handle_t sys_debug_transfer_handle(mx_handle_t proc, mx_handle_t src_handle) {
    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<ProcessDispatcher> process;
    mx_status_t status = up->GetDispatcher(proc, &process,
                                           MX_RIGHT_READ | MX_RIGHT_WRITE);
    if (status != NO_ERROR)
        return status;

    // Disallow this call on self.
    if (process.get() == up)
        return ERR_INVALID_ARGS;

    HandleUniquePtr handle = up->RemoveHandle(src_handle);
    if (!handle)
        return ERR_BAD_HANDLE;

    auto dest_hv = process->MapHandleToValue(handle.get());
    process->AddHandle(mxtl::move(handle));
    return dest_hv;
}

mx_ssize_t sys_debug_read_memory(mx_handle_t proc, uintptr_t vaddr, mx_size_t len, void* buffer) {
    if (!buffer)
        return ERR_INVALID_ARGS;
    if (len == 0 || len > kMaxDebugReadBlock)
        return ERR_INVALID_ARGS;

    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<ProcessDispatcher> process;
    mx_status_t status = up->GetDispatcher(proc, &process,
                                           MX_RIGHT_READ | MX_RIGHT_WRITE);
    if (status != NO_ERROR)
        return status;

    // Disallow this call on self.
    if (process.get() == up)
        return ERR_INVALID_ARGS;

    auto aspace = process->aspace();
    if (!aspace)
        return ERR_BAD_STATE;

    auto region = aspace->FindRegion(vaddr);
    if (!region)
        return ERR_NO_MEMORY;

    auto vmo = region->vmo();
    if (!vmo)
        return ERR_NO_MEMORY;

    uint64_t offset = vaddr - region->base() + region->object_offset();
    size_t read = 0;

    status_t st = vmo->ReadUser(buffer, offset, len, &read);

    if (st < 0)
        return st;

    return static_cast<mx_ssize_t>(read);
}

mx_ssize_t sys_ktrace_read(mx_handle_t handle, void* ptr, uint32_t off, uint32_t len) {
    // TODO: finer grained validation
    mx_status_t status;
    if ((status = validate_resource_handle(handle)) < 0) {
        return status;
    }

    return ktrace_read_user(ptr, off, len);
}

mx_status_t sys_ktrace_control(mx_handle_t handle, uint32_t action, uint32_t options) {
    // TODO: finer grained validation
    mx_status_t status;
    if ((status = validate_resource_handle(handle)) < 0) {
        return status;
    }

    return ktrace_control(action, options);
}

mx_ssize_t sys_debug_get_trace_vmos(mx_handle_t* _handles, size_t* _sizes, size_t elts) {
    extern struct list_node intel_pt_page_lists[SMP_MAX_CPUS];
    extern uint64_t intel_pt_trace_sizes[SMP_MAX_CPUS];

    mx_ssize_t count = 0;
    mx_handle_t handles[SMP_MAX_CPUS] = { 0 };
    size_t sizes[SMP_MAX_CPUS] = { 0 };

    auto up = ProcessDispatcher::GetCurrent();

    // TODO: cleanup properly on error
    for (uint i = 0; i < SMP_MAX_CPUS; ++i) {
        if (list_is_empty(&intel_pt_page_lists[i])) {
            continue;
        }

        // create a vm object
        mxtl::RefPtr<VmObject> vmo = VmObject::Create(0, 1ULL<<27);
        if (!vmo)
            return ERR_NO_MEMORY;

        size_t offset = 0;
        vm_page_t* p;
        list_for_every_entry (&intel_pt_page_lists[i], p, vm_page_t, node) {
            vmo->AddPage(p, offset);
            offset += PAGE_SIZE;
        }

        ASSERT(offset == 1ULL<<27);

        // create a Vm Object dispatcher
        mxtl::RefPtr<Dispatcher> dispatcher;
        mx_rights_t rights;
        mx_status_t result = VmObjectDispatcher::Create(mxtl::move(vmo), &dispatcher, &rights);
        if (result != NO_ERROR)
            return result;

        // create a handle and attach the dispatcher to it
        HandleUniquePtr handle(MakeHandle(mxtl::move(dispatcher), rights));
        if (!handle)
            return ERR_NO_MEMORY;

        mx_handle_t hv = up->MapHandleToValue(handle.get());
        up->AddHandle(mxtl::move(handle));

        handles[count] = hv;
        sizes[count] = intel_pt_trace_sizes[i];
        count++;
    }

    if ((size_t)count > elts) {
        return ERR_NOT_ENOUGH_BUFFER;
    }

    if (copy_to_user_unsafe(reinterpret_cast<uint8_t*>(_handles), reinterpret_cast<uint8_t*>(handles), sizeof(handles[0]) * count) != NO_ERROR)
        return ERR_INVALID_ARGS;

    if (copy_to_user_unsafe(reinterpret_cast<uint8_t*>(_sizes), reinterpret_cast<uint8_t*>(sizes), sizeof(sizes[0]) * count) != NO_ERROR)
        return ERR_INVALID_ARGS;

    return count;
}
