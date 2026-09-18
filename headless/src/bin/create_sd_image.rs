use std::path::PathBuf;

use anyhow::{Context, Result};
use clap::Parser;
use mofei_sim_headless::sd_image;

#[derive(Parser, Debug)]
#[command(version, about = "Create a raw FAT simulator SD image")]
struct Args {
    /// Source directory to copy into the FAT image.
    #[arg(long)]
    sd_root: PathBuf,

    /// Output raw image path.
    #[arg(long)]
    out: PathBuf,

    /// Raw image size in bytes.
    #[arg(long, default_value_t = sd_image::SD_IMAGE_BYTES)]
    bytes: u64,
}

fn main() -> Result<()> {
    let args = Args::parse();
    if !args.sd_root.exists() {
        anyhow::bail!("simulator SD root not found at {}", args.sd_root.display());
    }
    if let Some(parent) = args.out.parent() {
        std::fs::create_dir_all(parent)
            .with_context(|| format!("creating SD image parent {}", parent.display()))?;
    }
    sd_image::create_sd_image(&args.sd_root, &args.out, args.bytes)?;
    eprintln!(
        "[simulator-sd] wrote {} from {} ({} bytes)",
        args.out.display(),
        args.sd_root.display(),
        args.bytes
    );
    Ok(())
}
