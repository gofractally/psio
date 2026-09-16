//! Line-oriented validator used by tools/check-pjson-validation.py.
use std::io::{self, BufRead, Write};
fn main() -> io::Result<()> {
    let mut output = io::BufWriter::new(io::stdout().lock());
    for line in io::stdin().lock().lines() {
        let bytes = hex::decode(line?).expect("hex input");
        writeln!(output, "{}", u8::from(psio::pjson::validate(&bytes).is_ok()))?;
    }
    Ok(())
}
