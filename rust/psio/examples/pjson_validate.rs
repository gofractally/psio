//! Line-oriented pjson validator used by tools/check-pjson-validation.py.
//! One hex frame per line in; per line out: `invalid` (not a well-formed
//! frame), or `valid <c> <hex>|noform` — `<c>` is 1 when the frame is the
//! canonical one of its value, then the canonical frame, or `noform` when
//! the value has none (text that is not UTF-8).
use std::io::{self, BufRead, Write};
fn main() -> io::Result<()> {
    let mut output = io::BufWriter::new(io::stdout().lock());
    for line in io::stdin().lock().lines() {
        let bytes = hex::decode(line?).expect("hex input");
        if psio::pjson::lazy::validate(&bytes).is_err() {
            writeln!(output, "invalid")?;
            continue;
        }
        let canonical = u8::from(psio::pjson::validate(&bytes).is_ok());
        match psio::pjson::canonicalize(&bytes) {
            Ok(made) => writeln!(output, "valid {canonical} {}", hex::encode(made))?,
            Err(_) => writeln!(output, "valid {canonical} noform")?,
        }
    }
    Ok(())
}
