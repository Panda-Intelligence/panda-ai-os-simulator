use std::env;
use std::path::{Path, PathBuf};

pub const QEMU_RELATIVE_PATH: &str = ".qemu-cache/qemu/build/qemu-system-xtensa";

pub fn explicit_path_env(name: &str) -> Result<Option<PathBuf>, String> {
    match env::var_os(name) {
        None => Ok(None),
        Some(value) if value.is_empty() => Err(format!("{name} must not be empty")),
        Some(value) => Ok(Some(PathBuf::from(value))),
    }
}

#[allow(dead_code)]
pub fn integration_path() -> Result<Option<PathBuf>, String> {
    explicit_path_env("PANDA_SIMULATOR_INTEGRATION")
}

#[allow(dead_code)]
pub fn project_root() -> Result<Option<PathBuf>, String> {
    explicit_path_env("PANDA_SIMULATOR_PROJECT_ROOT")
}

pub fn qemu_path(cli_path: Option<&Path>, standalone_root: &Path) -> Result<PathBuf, String> {
    if let Some(path) = cli_path {
        return Ok(path.to_path_buf());
    }
    if let Some(path) = explicit_path_env("PANDA_SIMULATOR_QEMU")? {
        return Ok(path);
    }
    Ok(standalone_root.join(QEMU_RELATIVE_PATH))
}

/// Resolve test assets without coupling standalone runtime assets to a product.
#[allow(dead_code)]
pub fn fixture_root(explicit_project_root: Option<&Path>, standalone_root: &Path) -> PathBuf {
    explicit_project_root
        .unwrap_or(standalone_root)
        .to_path_buf()
}

pub fn project_path(
    value: &str,
    explicit_project_root: Option<&Path>,
    description: &str,
) -> Result<PathBuf, String> {
    let path = PathBuf::from(value);
    if path.is_absolute() {
        return Ok(path);
    }
    explicit_project_root
        .map(|root| root.join(path))
        .ok_or_else(|| {
            format!(
                "{description} '{value}' is relative; set PANDA_SIMULATOR_PROJECT_ROOT explicitly"
            )
        })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn relative_project_paths_require_an_explicit_root() {
        let error =
            project_path("apps/panda-os/device/build/panda_os.bin", None, "firmware").unwrap_err();
        assert!(error.contains("PANDA_SIMULATOR_PROJECT_ROOT"));
    }

    #[test]
    fn relative_project_paths_join_only_the_explicit_root() {
        let root = Path::new("/tmp/murphy");
        assert_eq!(
            project_path(
                "apps/panda-os/device/build/panda_os.bin",
                Some(root),
                "firmware"
            )
            .unwrap(),
            root.join("apps/panda-os/device/build/panda_os.bin")
        );
    }

    #[test]
    fn absolute_project_paths_do_not_need_a_root() {
        let path = Path::new("/tmp/firmware.bin");
        assert_eq!(
            project_path(path.to_str().unwrap(), None, "firmware").unwrap(),
            path
        );
    }

    #[test]
    fn fixture_root_uses_explicit_product_without_relocating_qemu() {
        let standalone = Path::new("/tmp/independent-simulator");
        let product = Path::new("/tmp/authorized-product");
        assert_eq!(fixture_root(Some(product), standalone), product);
        assert_eq!(
            qemu_path(None, standalone).unwrap(),
            standalone.join(QEMU_RELATIVE_PATH)
        );
    }

    #[test]
    fn fixture_root_without_product_preserves_standalone_layout() {
        let standalone = Path::new("/tmp/independent-simulator");
        assert_eq!(fixture_root(None, standalone), standalone);
    }

    #[test]
    fn qemu_defaults_to_the_standalone_root() {
        assert_eq!(
            qemu_path(None, Path::new("/tmp/simulator")).unwrap(),
            Path::new("/tmp/simulator").join(QEMU_RELATIVE_PATH)
        );
    }
}
