use std::io::{Seek, SeekFrom, Write};
use std::path::{Component, Path};
use std::{fs, io};

use anyhow::{anyhow, Context, Result};

pub const SD_SECTOR_BYTES: u64 = 512;
pub const SD_IMAGE_BYTES: u64 = 128 * 1024 * 1024;
pub const SD_CLUSTER_BYTES: u32 = 1024;

pub fn create_sd_image(sd_root: &Path, image_path: &Path, image_bytes: u64) -> Result<()> {
    if image_bytes % SD_SECTOR_BYTES != 0 {
        return Err(anyhow!(
            "SD image size {} must be divisible by sector size {}",
            image_bytes,
            SD_SECTOR_BYTES
        ));
    }

    let mut image = std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(true)
        .open(image_path)
        .with_context(|| format!("creating simulator SD image {}", image_path.display()))?;
    image
        .set_len(image_bytes)
        .with_context(|| format!("sizing simulator SD image {}", image_path.display()))?;

    let total_sectors = u32::try_from(image_bytes / SD_SECTOR_BYTES)
        .context("simulator SD image sector count does not fit in FAT32 formatter")?;
    fatfs::format_volume(
        &mut image,
        fatfs::FormatVolumeOptions::new()
            .fat_type(fatfs::FatType::Fat32)
            .bytes_per_sector(SD_SECTOR_BYTES as u16)
            .bytes_per_cluster(SD_CLUSTER_BYTES)
            .total_sectors(total_sectors)
            .volume_label(*b"MOFEI      "),
    )
    .with_context(|| format!("formatting simulator SD image {}", image_path.display()))?;

    image
        .seek(SeekFrom::Start(0))
        .with_context(|| format!("rewinding simulator SD image {}", image_path.display()))?;
    {
        let fs = fatfs::FileSystem::new(image, fatfs::FsOptions::new())
            .with_context(|| format!("mounting simulator SD image {}", image_path.display()))?;
        copy_dir_contents_to_fat(&fs.root_dir(), sd_root, sd_root).with_context(|| {
            format!(
                "copying simulator SD fixtures into {}",
                image_path.display()
            )
        })?;
    }

    Ok(())
}

pub fn sync_sd_image_to_dir(image_path: &Path, sd_root: &Path) -> Result<()> {
    let image = std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .open(image_path)
        .with_context(|| format!("opening simulator SD image {}", image_path.display()))?;
    let fs = fatfs::FileSystem::new(image, fatfs::FsOptions::new())
        .with_context(|| format!("mounting simulator SD image {}", image_path.display()))?;
    if sd_root.exists() {
        fs::remove_dir_all(sd_root)
            .with_context(|| format!("clearing simulator SD root {}", sd_root.display()))?;
    }
    fs::create_dir_all(sd_root)
        .with_context(|| format!("creating simulator SD root {}", sd_root.display()))?;
    let root_dir = fs.root_dir();
    copy_fat_dir_contents_to_dir(&root_dir, sd_root).with_context(|| {
        format!(
            "copying simulator SD image {} back into {}",
            image_path.display(),
            sd_root.display()
        )
    })
}

fn copy_dir_contents_to_fat<T: fatfs::ReadWriteSeek>(
    fat_root: &fatfs::Dir<'_, T>,
    source_root: &Path,
    current_dir: &Path,
) -> Result<()> {
    for entry in fs::read_dir(current_dir)
        .with_context(|| format!("reading simulator SD fixture dir {}", current_dir.display()))?
    {
        let entry = entry?;
        let source = entry.path();
        let fat_path = relative_fat_path(source_root, &source)?;
        let file_type = entry.file_type()?;
        if file_type.is_dir() {
            fat_root
                .create_dir(&fat_path)
                .with_context(|| format!("creating FAT dir {fat_path}"))?;
            copy_dir_contents_to_fat(fat_root, source_root, &source)?;
        } else if file_type.is_file() {
            let contents = fs::read(&source)
                .with_context(|| format!("reading simulator SD fixture {}", source.display()))?;
            let mut file = fat_root
                .create_file(&fat_path)
                .with_context(|| format!("creating FAT file {fat_path}"))?;
            file.truncate()
                .with_context(|| format!("truncating FAT file {fat_path}"))?;
            file.write_all(&contents)
                .with_context(|| format!("writing FAT file {fat_path}"))?;
        }
    }
    Ok(())
}

fn copy_fat_dir_contents_to_dir<T: fatfs::ReadWriteSeek>(
    fat_dir: &fatfs::Dir<'_, T>,
    target_dir: &Path,
) -> Result<()> {
    for entry in fat_dir.iter() {
        let entry = entry.with_context(|| format!("reading FAT dir {}", target_dir.display()))?;
        let entry_name = entry.file_name();
        if entry_name == "." || entry_name == ".." {
            continue;
        }
        let target = target_dir.join(&entry_name);
        if entry.is_dir() {
            fs::create_dir_all(&target)
                .with_context(|| format!("creating simulator SD dir {}", target.display()))?;
            copy_fat_dir_contents_to_dir(&entry.to_dir(), &target)?;
        } else if entry.is_file() {
            if let Some(parent) = target.parent() {
                fs::create_dir_all(parent).with_context(|| {
                    format!("creating simulator SD fixture parent {}", parent.display())
                })?;
            }
            let mut source_file = entry.to_file();
            let mut target_file = std::fs::File::create(&target)
                .with_context(|| format!("creating simulator SD fixture {}", target.display()))?;
            io::copy(&mut source_file, &mut target_file)
                .with_context(|| format!("copying FAT file to {}", target.display()))?;
        }
    }
    Ok(())
}

fn relative_fat_path(source_root: &Path, source: &Path) -> Result<String> {
    let relative = source.strip_prefix(source_root).with_context(|| {
        format!(
            "fixture path {} is not under {}",
            source.display(),
            source_root.display()
        )
    })?;
    let mut parts = Vec::new();
    for component in relative.components() {
        match component {
            Component::Normal(part) => {
                let part = part.to_str().ok_or_else(|| {
                    anyhow!(
                        "fixture path contains a non-UTF-8 component: {}",
                        source.display()
                    )
                })?;
                parts.push(part.to_string());
            }
            Component::CurDir => {}
            _ => {
                return Err(anyhow!(
                    "fixture path contains an unsupported component: {}",
                    source.display()
                ))
            }
        }
    }
    Ok(parts.join("/"))
}
