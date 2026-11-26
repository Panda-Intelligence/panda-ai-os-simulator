use anyhow::{anyhow, Context, Result};
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

const PARTITION_TABLE_OFFSET: usize = 0x8000;
const PARTITION_ENTRY_BYTES: usize = 32;

pub struct Lease {
    marker: PathBuf,
    remove_on_drop: bool,
}

impl Lease {
    pub fn acquire(path: &Path, sd_root: &Path) -> Result<Self> {
        match path.symlink_metadata() {
            Ok(metadata) if metadata.file_type().is_symlink() => {
                return Err(anyhow!(
                    "persistent flash image must not be a symbolic link"
                ));
            }
            Ok(_) => {}
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
            Err(error) => return Err(error).context("checking persistent flash image"),
        }
        let resolved = if path.exists() {
            path.canonicalize()?
        } else {
            path.parent()
                .filter(|parent| !parent.as_os_str().is_empty())
                .unwrap_or_else(|| Path::new("."))
                .canonicalize()?
                .join(
                    path.file_name()
                        .ok_or_else(|| anyhow!("persistent flash path has no filename"))?,
                )
        };
        if resolved.starts_with(sd_root.canonicalize()?) {
            return Err(anyhow!(
                "persistent flash image must be outside the simulated SD card"
            ));
        }
        let marker = resolved.with_extension("nvs.lock");
        fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&marker)
            .context("persistent flash is already leased, or its lock cannot be created")?;
        Ok(Self {
            marker,
            remove_on_drop: true,
        })
    }

    pub fn retain(&mut self) {
        self.remove_on_drop = false;
        eprintln!(
            "[headless] persistent flash lease retained at {}: guest exit was not confirmed; inspect running QEMU before removing the marker",
            self.marker.display()
        );
    }
}

impl Drop for Lease {
    fn drop(&mut self) {
        if self.remove_on_drop {
            let _ = fs::remove_file(&self.marker);
        }
    }
}

pub fn preserve_nvs(image: &mut [u8], table: &[u8], path: &Path) -> Result<()> {
    let mut region = None;
    let mut partitions = Vec::new();
    for entry in table.chunks_exact(PARTITION_ENTRY_BYTES) {
        if entry[..2] == [0xeb, 0xeb] || entry.iter().all(|byte| *byte == 0xff) {
            break;
        }
        if entry[..2] != [0xaa, 0x50] {
            return Err(anyhow!("invalid partition table for persistent NVS"));
        }
        let start = u32::from_le_bytes(entry[4..8].try_into().unwrap()) as usize;
        let size = u32::from_le_bytes(entry[8..12].try_into().unwrap()) as usize;
        let end = start
            .checked_add(size)
            .filter(|end| size != 0 && *end <= image.len())
            .ok_or_else(|| anyhow!("invalid partition bounds for persistent NVS"))?;
        partitions.push(start..end);
        let label = &entry[12..28];
        let label_end = label
            .iter()
            .position(|byte| *byte == 0)
            .unwrap_or(label.len());
        if entry[2..4] != [1, 2] || &label[..label_end] != b"nvs" {
            continue;
        }
        if region.is_some() {
            return Err(anyhow!("duplicate default NVS partition"));
        }
        if start % 4096 != 0 || size % 4096 != 0 {
            return Err(anyhow!("invalid default NVS partition bounds"));
        }
        if image[start..end].iter().any(|byte| *byte != 0xff) {
            return Err(anyhow!("default NVS partition overlaps firmware artifacts"));
        }
        region = Some(start..end);
    }
    let region =
        region.ok_or_else(|| anyhow!("persistent flash requires a default NVS partition"))?;
    if partitions
        .iter()
        .filter(|partition| partition.start < region.end && partition.end > region.start)
        .count()
        != 1
    {
        return Err(anyhow!("default NVS partition overlaps another partition"));
    }
    let table_end = PARTITION_TABLE_OFFSET
        .checked_add(table.len())
        .filter(|end| *end <= image.len())
        .ok_or_else(|| anyhow!("partition table exceeds flash image"))?;
    if region.start < table_end && region.end > PARTITION_TABLE_OFFSET {
        return Err(anyhow!("default NVS partition overlaps partition table"));
    }
    let old = match fs::read(path) {
        Ok(bytes) => bytes,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(()),
        Err(error) => return Err(error).context("reading persistent flash image"),
    };
    if old.len() != image.len() {
        return Err(anyhow!(
            "persistent flash image size differs from firmware flash size"
        ));
    }
    if old[PARTITION_TABLE_OFFSET..table_end] != *table {
        return Err(anyhow!(
            "persistent flash partition table differs from firmware"
        ));
    }
    image[region.clone()].copy_from_slice(&old[region]);
    Ok(())
}

pub fn write_image(path: &Path, image: &[u8]) -> Result<()> {
    let staging = path.with_extension("nvs.tmp");
    let mut file = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&staging)
        .context("creating persistent flash replacement")?;
    let result = (|| -> Result<()> {
        file.write_all(image)?;
        file.sync_all()?;
        fs::rename(&staging, path)?;
        Ok(())
    })();
    if result.is_err() {
        let _ = fs::remove_file(staging);
    }
    result.context("replacing persistent flash image")
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fixture() -> (Vec<u8>, Vec<u8>, std::path::PathBuf) {
        let mut image = vec![0xff; 0x20000];
        let mut table = vec![0xff; 64];
        table[..32].fill(0);
        table[..4].copy_from_slice(&[0xaa, 0x50, 1, 2]);
        table[4..8].copy_from_slice(&0x9000_u32.to_le_bytes());
        table[8..12].copy_from_slice(&0x6000_u32.to_le_bytes());
        table[12..15].copy_from_slice(b"nvs");
        image[0x8000..0x8040].copy_from_slice(&table);
        let path = std::env::temp_dir().join(format!(
            "panda-native-nvs-{}-{:?}.img",
            std::process::id(),
            std::thread::current().id()
        ));
        let _ = fs::remove_file(&path);
        (image, table, path)
    }

    #[test]
    fn missing_image_stays_erased_then_only_nvs_survives_relaunch() {
        let (mut image, table, path) = fixture();
        preserve_nvs(&mut image, &table, &path).unwrap();
        assert!(image[0x9000..0xf000].iter().all(|byte| *byte == 0xff));
        image[0x9000..0xf000].fill(0x73);
        image[0x10000..0x11000].fill(0x45);
        fs::write(&path, &image).unwrap();
        let mut next = vec![0xff; image.len()];
        next[0x8000..0x8040].copy_from_slice(&table);
        preserve_nvs(&mut next, &table, &path).unwrap();
        assert_eq!(&next[0x9000..0xf000], &image[0x9000..0xf000]);
        assert!(next[0x10000..0x11000].iter().all(|byte| *byte == 0xff));
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn wrong_size_or_partition_table_never_resets_saved_nvs() {
        let (mut image, table, path) = fixture();
        fs::write(&path, b"truncated").unwrap();
        assert!(preserve_nvs(&mut image, &table, &path).is_err());
        assert_eq!(fs::read(&path).unwrap(), b"truncated");
        let mut old = image.clone();
        old[0x8008] ^= 1;
        fs::write(&path, &old).unwrap();
        assert!(preserve_nvs(&mut image, &table, &path).is_err());
        assert_eq!(fs::read(&path).unwrap(), old);
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn requires_real_bounded_non_overlapping_default_nvs_partition() {
        let (image, table, path) = fixture();
        let mut invalid = table.clone();
        invalid[12..15].copy_from_slice(b"phy");
        assert!(preserve_nvs(&mut image.clone(), &invalid, &path).is_err());
        for (offset, value) in [(4, 0x9001_u32), (8, 0), (8, 0x20000)] {
            let mut invalid = table.clone();
            invalid[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
            assert!(preserve_nvs(&mut image.clone(), &invalid, &path).is_err());
        }
        let mut occupied = image.clone();
        occupied[0x9000] = 0xe9;
        assert!(preserve_nvs(&mut occupied, &table, &path).is_err());
        let mut duplicate = table.clone();
        duplicate[32..64].copy_from_slice(&table[..32]);
        assert!(preserve_nvs(&mut image.clone(), &duplicate, &path).is_err());
    }

    #[test]
    fn rejects_other_declared_partitions_overlapping_erased_nvs_bytes() {
        let (mut image, mut table, path) = fixture();
        table[32..64].fill(0);
        table[32..36].copy_from_slice(&[0xaa, 0x50, 1, 0x81]);
        table[36..40].copy_from_slice(&0xa000_u32.to_le_bytes());
        table[40..44].copy_from_slice(&0x1000_u32.to_le_bytes());
        table[44..51].copy_from_slice(b"storage");
        assert!(preserve_nvs(&mut image, &table, &path).is_err());
        assert!(!path.exists());
    }

    #[test]
    fn lease_prevents_two_writers_and_an_sd_backed_flash_image() {
        let (_, _, path) = fixture();
        let sd_root = path.with_extension("sd");
        fs::create_dir_all(&sd_root).unwrap();
        let first = Lease::acquire(&path, &sd_root).unwrap();
        assert!(Lease::acquire(&path, &sd_root).is_err());
        drop(first);
        assert!(Lease::acquire(&path, &sd_root).is_ok());
        assert!(Lease::acquire(&sd_root.join("flash.img"), &sd_root).is_err());
        fs::remove_dir_all(sd_root).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn lease_rejects_a_dangling_symlink_into_the_sd_card() {
        let (_, _, path) = fixture();
        let sd_root = path.with_extension("sd");
        fs::create_dir_all(&sd_root).unwrap();
        let link = path.with_extension("link");
        std::os::unix::fs::symlink(sd_root.join("flash.img"), &link).unwrap();
        let rejected = Lease::acquire(&link, &sd_root).is_err();
        fs::remove_file(link).unwrap();
        fs::remove_dir_all(sd_root).unwrap();
        assert!(rejected);
    }

    #[test]
    fn replacement_failure_preserves_old_image_and_success_replaces_it() {
        let (_, _, path) = fixture();
        let staging = path.with_extension("nvs.tmp");
        fs::write(&path, b"old NVS").unwrap();
        fs::write(&staging, b"another preparation").unwrap();
        assert!(write_image(&path, b"new NVS").is_err());
        assert_eq!(fs::read(&path).unwrap(), b"old NVS");
        assert_eq!(fs::read(&staging).unwrap(), b"another preparation");
        fs::remove_file(&staging).unwrap();
        write_image(&path, b"new NVS").unwrap();
        assert_eq!(fs::read(&path).unwrap(), b"new NVS");
        assert!(!staging.exists());
        fs::remove_file(path).unwrap();
    }
}
