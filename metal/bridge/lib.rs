//! sweetmtl-bridge: plain C functions over metal2vulkan-runtime, for SweetMTL.bundle's Objective-C classes.
//!
//! Every object goes out as an owned pointer (`Box<T>`, freed by its `smtl_*_release`); calls return 0 for
//! success or a negative number, and the last error's text is kept per thread (`smtl_last_error`).
//!
//! The Vulkan driver: iOS has no Vulkan loader, so the runtime is told (DeviceOptions::loader) to open this
//! bundle's own binary, whose exported `vkGetInstanceProcAddr` below hands every request to Turnip's
//! `vk_icdGetInstanceProcAddr` (libvulkan_freedreno.dylib, opened by `smtl_device_new`).

use metal2vulkan_runtime::{BlitCommandEncoder, Buffer, CommandBuffer, CommandQueue, Device, DeviceOptions, ResourceOptions};
use std::cell::RefCell;
use std::ffi::{c_char, c_void, CStr, CString};
use std::path::PathBuf;
use std::ptr;
use std::sync::OnceLock;

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::default());
}

fn fail<E: std::fmt::Display>(what: &str, e: E) -> i32 {
    let text = format!("{what}: {e}");
    LAST_ERROR.with(|l| *l.borrow_mut() = CString::new(text.replace('\0', " ")).unwrap_or_default());
    -1
}

/// The last error of this thread, or "" (valid until the thread's next failing call).
#[no_mangle]
pub extern "C" fn smtl_last_error() -> *const c_char {
    LAST_ERROR.with(|l| l.borrow().as_ptr())
}

// ---- the Vulkan entry point (this binary acts as the runtime's "loader") ----

type Gipa = unsafe extern "C" fn(*mut c_void, *const c_char) -> *mut c_void;
static TURNIP_GIPA: OnceLock<usize> = OnceLock::new();

/// Turnip's vk_icdGetInstanceProcAddr, for every instance-level and global request.
#[no_mangle]
pub unsafe extern "C" fn vkGetInstanceProcAddr(instance: *mut c_void, name: *const c_char) -> *mut c_void {
    match TURNIP_GIPA.get() {
        Some(&f) => {
            let f: Gipa = std::mem::transmute(f);
            f(instance, name)
        }
        None => ptr::null_mut(),
    }
}

fn open_turnip(path: &CStr) -> Result<(), String> {
    if TURNIP_GIPA.get().is_some() {
        return Ok(());
    }
    // SAFETY: plain dlopen/dlsym of a C library path.
    unsafe {
        let h = libc::dlopen(path.as_ptr(), libc::RTLD_NOW | libc::RTLD_LOCAL);
        if h.is_null() {
            let e = libc::dlerror();
            let why = if e.is_null() { "?".into() } else { CStr::from_ptr(e).to_string_lossy().into_owned() };
            return Err(format!("dlopen {}: {why}", path.to_string_lossy()));
        }
        let f = libc::dlsym(h, c"vk_icdGetInstanceProcAddr".as_ptr());
        if f.is_null() {
            return Err(format!("no vk_icdGetInstanceProcAddr in {}", path.to_string_lossy()));
        }
        let _ = TURNIP_GIPA.set(f as usize);
    }
    Ok(())
}

/// The path of the binary that holds this code (the bundle's executable): the runtime's "loader".
fn own_binary() -> Option<PathBuf> {
    // SAFETY: dladdr on a function of this image.
    unsafe {
        let mut info: libc::Dl_info = std::mem::zeroed();
        if libc::dladdr(vkGetInstanceProcAddr as *const c_void, &mut info) == 0 || info.dli_fname.is_null() {
            return None;
        }
        Some(PathBuf::from(CStr::from_ptr(info.dli_fname).to_string_lossy().into_owned()))
    }
}

// ---- device ----

/// A device on Turnip at `turnip` (the path of libvulkan_freedreno.dylib); `cache` is the shader cache
/// directory or NULL for the runtime's default. NULL on failure (smtl_last_error says why).
#[no_mangle]
pub unsafe extern "C" fn smtl_device_new(turnip: *const c_char, cache: *const c_char) -> *mut Device {
    sweetmtl_add_device_protocols(); // before MTLAddDevice checks the new device (and no load-time hook needed)
    if turnip.is_null() {
        fail("smtl_device_new", "no Turnip path");
        return ptr::null_mut();
    }
    if let Err(e) = open_turnip(CStr::from_ptr(turnip)) {
        fail("smtl_device_new", e);
        return ptr::null_mut();
    }
    let mut options = DeviceOptions::default();
    options.loader = own_binary();
    if !cache.is_null() {
        options.shader_cache.directory = Some(PathBuf::from(CStr::from_ptr(cache).to_string_lossy().into_owned()));
    }
    match Device::system_default_with(&options) {
        Ok(d) => Box::into_raw(Box::new(d)),
        Err(e) => {
            fail("Device::system_default_with", e);
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn smtl_device_release(d: *mut Device) {
    if !d.is_null() {
        drop(Box::from_raw(d));
    }
}

/// The device's name into `out` (NUL-terminated, cut to `len`).
#[no_mangle]
pub unsafe extern "C" fn smtl_device_name(d: *const Device, out: *mut c_char, len: usize) {
    if d.is_null() || out.is_null() || len == 0 {
        return;
    }
    let name = (*d).name().as_bytes();
    let n = name.len().min(len - 1);
    ptr::copy_nonoverlapping(name.as_ptr(), out as *mut u8, n);
    *out.add(n) = 0;
}

#[no_mangle]
pub unsafe extern "C" fn smtl_device_registry_id(d: *const Device) -> u64 {
    if d.is_null() { 0 } else { (*d).registry_id() }
}

// ---- queue, buffers ----

#[no_mangle]
pub unsafe extern "C" fn smtl_queue_new(d: *const Device) -> *mut CommandQueue {
    match (*d).new_command_queue() {
        Ok(q) => Box::into_raw(Box::new(q)),
        Err(e) => {
            fail("new_command_queue", e);
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn smtl_queue_release(q: *mut CommandQueue) {
    if !q.is_null() {
        drop(Box::from_raw(q));
    }
}

/// MTLResourceOptions: storage mode in bits 4-7 (0 shared, 1 managed, 2 private, 3 memoryless).
fn resource_options(raw: u64) -> ResourceOptions {
    match (raw >> 4) & 0xf {
        2 => ResourceOptions::private(),
        _ => ResourceOptions::shared(),
    }
}

#[no_mangle]
pub unsafe extern "C" fn smtl_buffer_new(d: *const Device, length: u64, options: u64) -> *mut Buffer {
    match (*d).new_buffer(length, resource_options(options)) {
        Ok(b) => Box::into_raw(Box::new(b)),
        Err(e) => {
            fail("new_buffer", e);
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn smtl_buffer_release(b: *mut Buffer) {
    if !b.is_null() {
        drop(Box::from_raw(b));
    }
}

/// The CPU's view of a shared buffer, or NULL (private).
#[no_mangle]
pub unsafe extern "C" fn smtl_buffer_contents(b: *const Buffer) -> *mut c_void {
    match (*b).contents() {
        Some(p) => p.as_ptr() as *mut c_void,
        None => ptr::null_mut(),
    }
}

#[no_mangle]
pub unsafe extern "C" fn smtl_buffer_length(b: *const Buffer) -> u64 {
    (*b).length()
}

// ---- command buffers ----

#[no_mangle]
pub unsafe extern "C" fn smtl_cmdbuf_new(q: *const CommandQueue) -> *mut CommandBuffer {
    match (*q).command_buffer() {
        Ok(c) => Box::into_raw(Box::new(c)),
        Err(e) => {
            fail("command_buffer", e);
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn smtl_cmdbuf_release(c: *mut CommandBuffer) {
    if !c.is_null() {
        drop(Box::from_raw(c));
    }
}

#[no_mangle]
pub unsafe extern "C" fn smtl_cmdbuf_commit(c: *const CommandBuffer) -> i32 {
    match (*c).commit() {
        Ok(()) => 0,
        Err(e) => fail("commit", e),
    }
}

#[no_mangle]
pub unsafe extern "C" fn smtl_cmdbuf_wait(c: *const CommandBuffer) -> i32 {
    match (*c).wait_until_completed() {
        Ok(()) => 0,
        Err(e) => fail("wait_until_completed", e),
    }
}

/// MTLCommandBufferStatus as Metal numbers it (0 not enqueued ... 4 completed, 5 error).
#[no_mangle]
pub unsafe extern "C" fn smtl_cmdbuf_status(c: *const CommandBuffer) -> u64 {
    use metal2vulkan_runtime::CommandBufferStatus as S;
    match (*c).status() {
        S::NotEnqueued => 0,
        S::Enqueued => 1,
        S::Committed => 2,
        S::Scheduled => 3,
        S::Completed => 4,
        _ => 5,
    }
}

// ---- blit ----

#[no_mangle]
pub unsafe extern "C" fn smtl_blit_new(c: *const CommandBuffer) -> *mut BlitCommandEncoder {
    match (*c).blit_command_encoder() {
        Ok(b) => Box::into_raw(Box::new(b)),
        Err(e) => {
            fail("blit_command_encoder", e);
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn smtl_blit_fill(e: *mut BlitCommandEncoder, b: *const Buffer, start: u64, end: u64, value: u8) -> i32 {
    match (*e).fill_buffer(&*b, start..end, value) {
        Ok(()) => 0,
        Err(err) => fail("fill_buffer", err),
    }
}

/// endEncoding: consumes the encoder (no release afterwards).
#[no_mangle]
pub unsafe extern "C" fn smtl_blit_end(e: *mut BlitCommandEncoder) -> i32 {
    match Box::from_raw(e).end_encoding() {
        Ok(()) => 0,
        Err(err) => fail("end_encoding", err),
    }
}

// ---- Metal.framework's checks on a plugin device ----

extern "C" {
    fn objc_getClass(name: *const c_char) -> *mut c_void;
    fn objc_getProtocol(name: *const c_char) -> *mut c_void;
    fn class_addProtocol(cls: *mut c_void, protocol: *mut c_void) -> bool;
}

/// MTLAddDevice (Metal 16.5) asserts that a new device conforms to MTLDevice and to the private MTLDeviceSPI.
/// SweetMTLDevice lists MTLDevice in its source; MTLDeviceSPI has no header, so it is added here, when the bundle
/// loads (seen 2026-11-13: "failed assertion [newDevice conformsToProtocol:@protocol(MTLDeviceSPI)]").
#[no_mangle]
pub extern "C" fn sweetmtl_add_device_protocols() {
    // SAFETY: plain Objective-C runtime calls with NUL-terminated names; a missing class or protocol is skipped.
    unsafe {
        let cls = objc_getClass(c"SweetMTLDevice".as_ptr());
        for name in [c"MTLDevice", c"MTLDeviceSPI"] {
            let p = objc_getProtocol(name.as_ptr());
            if !cls.is_null() && !p.is_null() {
                class_addProtocol(cls, p);
            }
        }
    }
}

#[used]
#[link_section = "__DATA,__mod_init_func"]
static ADD_DEVICE_PROTOCOLS: extern "C" fn() = sweetmtl_add_device_protocols;

// ---- libraries, functions, compute pipelines (milestone 2) ----

use metal2vulkan_runtime::{ComputeCommandEncoder, ComputePipelineState, Function, Library, PipelineOptions, Size};

/// A library from a metallib's bytes (newLibraryWithData:).
#[no_mangle]
pub unsafe extern "C" fn smtl_library_new_data(d: *const Device, bytes: *const u8, len: usize) -> *mut Library {
    match (*d).new_library_with_data(std::slice::from_raw_parts(bytes, len)) {
        Ok(l) => Box::into_raw(Box::new(l)),
        Err(e) => {
            fail("new_library_with_data", e);
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn smtl_library_release(l: *mut Library) {
    if !l.is_null() {
        drop(Box::from_raw(l));
    }
}

#[no_mangle]
pub unsafe extern "C" fn smtl_function_new(l: *const Library, name: *const c_char) -> *mut Function {
    let name = CStr::from_ptr(name).to_string_lossy();
    match (*l).new_function_with_name(&name) {
        Ok(f) => Box::into_raw(Box::new(f)),
        Err(e) => {
            fail("new_function_with_name", e);
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn smtl_function_release(f: *mut Function) {
    if !f.is_null() {
        drop(Box::from_raw(f));
    }
}

#[no_mangle]
pub unsafe extern "C" fn smtl_compute_pipeline_new(d: *const Device, f: *const Function) -> *mut ComputePipelineState {
    match (*d).new_compute_pipeline_state_with_function(&*f, &PipelineOptions::default()) {
        Ok(p) => Box::into_raw(Box::new(p)),
        Err(e) => {
            fail("new_compute_pipeline_state_with_function", e);
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn smtl_compute_pipeline_release(p: *mut ComputePipelineState) {
    if !p.is_null() {
        drop(Box::from_raw(p));
    }
}

#[no_mangle]
pub unsafe extern "C" fn smtl_compute_pipeline_max_threads(p: *const ComputePipelineState) -> u32 {
    (*p).max_total_threads_per_threadgroup()
}

#[no_mangle]
pub unsafe extern "C" fn smtl_compute_pipeline_width(p: *const ComputePipelineState) -> u32 {
    (*p).thread_execution_width()
}

#[no_mangle]
pub unsafe extern "C" fn smtl_compute_new(c: *const CommandBuffer) -> *mut ComputeCommandEncoder {
    match (*c).compute_command_encoder() {
        Ok(e) => Box::into_raw(Box::new(e)),
        Err(e) => {
            fail("compute_command_encoder", e);
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn smtl_compute_set_pipeline(e: *mut ComputeCommandEncoder, p: *const ComputePipelineState) -> i32 {
    match (*e).set_compute_pipeline_state(&*p) {
        Ok(()) => 0,
        Err(err) => fail("set_compute_pipeline_state", err),
    }
}

/// setBuffer:offset:atIndex: (b may be NULL).
#[no_mangle]
pub unsafe extern "C" fn smtl_compute_set_buffer(e: *mut ComputeCommandEncoder, b: *const Buffer, offset: u64, index: u32) -> i32 {
    let b = if b.is_null() { None } else { Some(&*b) };
    match (*e).set_buffer(b, offset, index) {
        Ok(()) => 0,
        Err(err) => fail("set_buffer", err),
    }
}

#[no_mangle]
pub unsafe extern "C" fn smtl_compute_set_bytes(e: *mut ComputeCommandEncoder, bytes: *const u8, len: usize, index: u32) -> i32 {
    match (*e).set_bytes(std::slice::from_raw_parts(bytes, len), index) {
        Ok(()) => 0,
        Err(err) => fail("set_bytes", err),
    }
}

#[no_mangle]
pub unsafe extern "C" fn smtl_compute_dispatch_groups(e: *mut ComputeCommandEncoder, gx: u64, gy: u64, gz: u64, tx: u64, ty: u64, tz: u64) -> i32 {
    match (*e).dispatch_threadgroups(Size::new(gx, gy, gz), Size::new(tx, ty, tz)) {
        Ok(()) => 0,
        Err(err) => fail("dispatch_threadgroups", err),
    }
}

#[no_mangle]
pub unsafe extern "C" fn smtl_compute_dispatch_threads(e: *mut ComputeCommandEncoder, x: u64, y: u64, z: u64, tx: u64, ty: u64, tz: u64) -> i32 {
    match (*e).dispatch_threads(Size::new(x, y, z), Size::new(tx, ty, tz)) {
        Ok(()) => 0,
        Err(err) => fail("dispatch_threads", err),
    }
}

/// endEncoding: consumes the encoder.
#[no_mangle]
pub unsafe extern "C" fn smtl_compute_end(e: *mut ComputeCommandEncoder) -> i32 {
    match Box::from_raw(e).end_encoding() {
        Ok(()) => 0,
        Err(err) => fail("end_encoding", err),
    }
}
