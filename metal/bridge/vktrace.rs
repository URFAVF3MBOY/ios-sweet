//! SWEETMTL_VKTRACE=1: the bridge, as the runtime's Vulkan loader, hands out timing wrappers for the
//! submit and wait entry points; each call prints its start (ms since the first traced call), its
//! length and its thread. For finding where a command buffer's time goes.

use std::ffi::{c_char, c_void, CStr};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::OnceLock;
use std::time::Instant;

type H = *mut c_void;

fn base() -> &'static Instant {
    static B: OnceLock<Instant> = OnceLock::new();
    B.get_or_init(Instant::now)
}

pub fn on() -> bool {
    static ON: OnceLock<bool> = OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("SWEETMTL_VKTRACE").is_some())
}

fn tid() -> u64 {
    let mut t = 0u64;
    // SAFETY: pthread_threadid_np(NULL, &t) reads this thread's id.
    unsafe { libc::pthread_threadid_np(0 as libc::pthread_t, &mut t) };
    t
}

fn log(name: &str, t0: Instant, extra: String, r: i32) {
    let start = t0.duration_since(*base()).as_secs_f64() * 1000.0;
    let len = t0.elapsed().as_secs_f64() * 1000.0;
    eprintln!("vk {start:10.3} +{len:7.3} ms t{} {name}{extra} -> {r}", tid() & 0xffff);
}

macro_rules! slot {
    ($s:ident) => {
        static $s: AtomicUsize = AtomicUsize::new(0);
    };
}
slot!(QUEUE_SUBMIT);
slot!(QUEUE_SUBMIT2);
slot!(WAIT_FENCES);
slot!(WAIT_SEMAPHORES);
slot!(COUNTER_VALUE);
slot!(FENCE_STATUS);
slot!(GET_DEVICE_PROC_ADDR);

unsafe extern "C" fn queue_submit(q: H, n: u32, s: *const c_void, f: u64) -> i32 {
    let real: unsafe extern "C" fn(H, u32, *const c_void, u64) -> i32 = std::mem::transmute(QUEUE_SUBMIT.load(Ordering::Relaxed));
    let t0 = Instant::now();
    let r = real(q, n, s, f);
    log("vkQueueSubmit", t0, format!(" n={n} fence={}", f != 0), r);
    r
}

unsafe extern "C" fn queue_submit2(q: H, n: u32, s: *const c_void, f: u64) -> i32 {
    let real: unsafe extern "C" fn(H, u32, *const c_void, u64) -> i32 = std::mem::transmute(QUEUE_SUBMIT2.load(Ordering::Relaxed));
    let t0 = Instant::now();
    let r = real(q, n, s, f);
    log("vkQueueSubmit2", t0, format!(" n={n} fence={}", f != 0), r);
    r
}

unsafe extern "C" fn wait_fences(d: H, n: u32, f: *const u64, all: u32, timeout: u64) -> i32 {
    let real: unsafe extern "C" fn(H, u32, *const u64, u32, u64) -> i32 = std::mem::transmute(WAIT_FENCES.load(Ordering::Relaxed));
    let t0 = Instant::now();
    let r = real(d, n, f, all, timeout);
    log("vkWaitForFences", t0, format!(" n={n} timeout={timeout}"), r);
    r
}

#[repr(C)]
struct SemaphoreWaitInfo {
    s_type: u32,
    p_next: *const c_void,
    flags: u32,
    count: u32,
    semaphores: *const u64,
    values: *const u64,
}

unsafe extern "C" fn wait_semaphores(d: H, info: *const SemaphoreWaitInfo, timeout: u64) -> i32 {
    let real: unsafe extern "C" fn(H, *const SemaphoreWaitInfo, u64) -> i32 = std::mem::transmute(WAIT_SEMAPHORES.load(Ordering::Relaxed));
    let i = &*info;
    let v = if i.count > 0 { *i.values } else { 0 };
    let t0 = Instant::now();
    let r = real(d, info, timeout);
    log("vkWaitSemaphores", t0, format!(" n={} value={v} timeout={timeout}", i.count), r);
    r
}

unsafe extern "C" fn counter_value(d: H, s: u64, out: *mut u64) -> i32 {
    let real: unsafe extern "C" fn(H, u64, *mut u64) -> i32 = std::mem::transmute(COUNTER_VALUE.load(Ordering::Relaxed));
    let t0 = Instant::now();
    let r = real(d, s, out);
    log("vkGetSemaphoreCounterValue", t0, format!(" = {}", *out), r);
    r
}

unsafe extern "C" fn fence_status(d: H, f: u64) -> i32 {
    let real: unsafe extern "C" fn(H, u64) -> i32 = std::mem::transmute(FENCE_STATUS.load(Ordering::Relaxed));
    let t0 = Instant::now();
    let r = real(d, f);
    log("vkGetFenceStatus", t0, String::new(), r);
    r
}

/// The wrapper for `name` if it is traced (saving `real`), else `real`.
unsafe fn wrap(name: &CStr, real: *mut c_void) -> *mut c_void {
    if real.is_null() {
        return real;
    }
    let (slot, w): (&AtomicUsize, *mut c_void) = match name.to_bytes() {
        b"vkQueueSubmit" => (&QUEUE_SUBMIT, queue_submit as *mut c_void),
        b"vkQueueSubmit2" | b"vkQueueSubmit2KHR" => (&QUEUE_SUBMIT2, queue_submit2 as *mut c_void),
        b"vkWaitForFences" => (&WAIT_FENCES, wait_fences as *mut c_void),
        b"vkWaitSemaphores" | b"vkWaitSemaphoresKHR" => (&WAIT_SEMAPHORES, wait_semaphores as *mut c_void),
        b"vkGetSemaphoreCounterValue" | b"vkGetSemaphoreCounterValueKHR" => (&COUNTER_VALUE, counter_value as *mut c_void),
        b"vkGetFenceStatus" => (&FENCE_STATUS, fence_status as *mut c_void),
        b"vkGetDeviceProcAddr" => (&GET_DEVICE_PROC_ADDR, get_device_proc_addr as *mut c_void),
        _ => return real,
    };
    slot.store(real as usize, Ordering::Relaxed);
    let _ = base();
    w
}

unsafe extern "C" fn get_device_proc_addr(d: H, name: *const c_char) -> *mut c_void {
    let real: unsafe extern "C" fn(H, *const c_char) -> *mut c_void = std::mem::transmute(GET_DEVICE_PROC_ADDR.load(Ordering::Relaxed));
    wrap(CStr::from_ptr(name), real(d, name))
}

/// Called by the bridge's vkGetInstanceProcAddr with Turnip's answer.
pub unsafe fn instance_proc(name: *const c_char, real: *mut c_void) -> *mut c_void {
    if !on() || name.is_null() {
        return real;
    }
    wrap(CStr::from_ptr(name), real)
}
