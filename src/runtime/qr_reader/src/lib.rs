//! The C entry points to the QR reader. The caller owns the memory, so that it can be released for
//! the signing step.
#![cfg_attr(target_os = "none", no_std)]

use bitwasm_signer_qr as qr;
use core::mem::size_of;
use core::slice;

// The browser demo has std's panic, which aborts
#[cfg(target_os = "none")]
extern "C" {
    fn qr_reader_panic() -> !;
}

#[cfg(target_os = "none")]
#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    unsafe { qr_reader_panic() }
}

#[no_mangle]
pub extern "C" fn qr_reader_state_size() -> usize {
    size_of::<qr::State>()
}

#[no_mangle]
pub extern "C" fn qr_reader_output_cap() -> usize {
    qr::OUTPUT_CAP
}

/// # Safety
/// `state` points to `qr_reader_state_size()` writable bytes, aligned for the state.
#[no_mangle]
pub unsafe extern "C" fn qr_reader_state_init(state: *mut qr::State) {
    state.write(qr::State::ZERO);
}

/// The length read into `out`, or a code as `qr::read_within` gives: 0 for no code, -1 for one that
/// did not decode, -3 for one of the wrong kind.
///
/// # Safety
/// `state` is initialised, `px` holds `w * h` bytes and `out` holds `qr_reader_output_cap()` bytes.
#[no_mangle]
pub unsafe extern "C" fn qr_reader_decode(
    state: *mut qr::State,
    px: *const u8,
    w: usize,
    h: usize,
    seedqr: bool,
    animated: bool,
    out: *mut u8,
    budget: u32,
) -> i32 {
    let kind = if seedqr {
        qr::Kind::SeedQr
    } else if animated {
        qr::Kind::Animated
    } else {
        qr::Kind::Any
    };
    let px = slice::from_raw_parts(px, w * h);
    let out = slice::from_raw_parts_mut(out, qr::OUTPUT_CAP);
    qr::read_within(px, w, h, kind, out, &mut *state, budget)
}
