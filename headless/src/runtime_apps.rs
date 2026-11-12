// Official Runtime App seeding for QEMU E2E cases (`boot.runtimeApps`).
//
// A case names an app authoring directory (for example
// `apps/panda-developers/runtime-apps/official/panda-buzzer-memory`). The runner turns it into a schema-v2 package
// the way a Store entry would be built: the authoring manifest's capabilities become the declared entitlements and its
// budgets stay in the manifest. The package is then packed, signed and installed into the SD root by the real
// `apps/toolchain` CLI, so the firmware installs, verifies, grants and launches it through its own v2 path.
//
// The firmware only accepts packages signed under its configured trust root. The shipped firmware has none, so this
// needs a simulator test firmware built with a throwaway root (`apps/panda-os/tools/simulator-app-trust.sh`) and that
// root's key passed explicitly through `--runtime-app-root-key` or PANDA_SIMULATOR_RUNTIME_APP_ROOT_KEY. Nothing is
// guessed and no grant is written here: entitlement consent stays a visible step of the case.

use std::collections::HashSet;
use std::fs;
use std::path::{Component, Path, PathBuf};

use anyhow::{anyhow, Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::{json, Map, Value};

use super::{
    read_toolchain_fixture_key, repo_root_path, run_toolchain_fixture_command,
    stable_socket_suffix_hash,
};

pub(crate) const ROOT_KEY_ENV: &str = "PANDA_SIMULATOR_RUNTIME_APP_ROOT_KEY";

#[derive(Clone, Debug, Deserialize, Serialize)]
pub(crate) struct E2EBootRuntimeApp {
    /// Authoring directory: repository-relative (resolved against the fixture root) or absolute.
    pub dir: String,
}

pub(crate) fn validate_boot_runtime_apps(apps: &[E2EBootRuntimeApp]) -> Result<()> {
    let mut seen = HashSet::new();
    for (index, app) in apps.iter().enumerate() {
        let dir = app.dir.trim();
        if dir.is_empty() {
            return Err(anyhow!(
                "boot.runtimeApps entry {} has an empty dir",
                index + 1
            ));
        }
        if !Path::new(dir).is_absolute()
            && Path::new(dir)
                .components()
                .any(|part| !matches!(part, Component::Normal(_)))
        {
            return Err(anyhow!(
                "boot.runtimeApps entry {} dir '{}' must be absolute or stay inside the repository root",
                index + 1,
                dir
            ));
        }
        if !seen.insert(dir.to_string()) {
            return Err(anyhow!("boot.runtimeApps lists '{}' twice", dir));
        }
    }
    Ok(())
}

/// Builds the schema-v2 authoring manifest from an official app's v1 authoring `manifest.json`.
/// A manifest that is already schema v2 is returned unchanged.
pub(crate) fn schema_v2_manifest(authoring: &Value) -> Result<Value> {
    let object = authoring
        .as_object()
        .ok_or_else(|| anyhow!("authoring manifest.json is not an object"))?;
    if object.get("schemaVersion").and_then(Value::as_u64) == Some(2) {
        return Ok(authoring.clone());
    }
    let text = |key: &str| -> Result<String> {
        object
            .get(key)
            .and_then(Value::as_str)
            .filter(|value| !value.is_empty())
            .map(str::to_string)
            .ok_or_else(|| anyhow!("authoring manifest.json needs a string `{}`", key))
    };
    let mut entitlements: Vec<String> = object
        .get("capabilities")
        .and_then(Value::as_object)
        .map(|caps| {
            caps.iter()
                .filter(|(_, enabled)| enabled.as_bool() == Some(true))
                .map(|(token, _)| token.clone())
                .collect()
        })
        .unwrap_or_default();
    entitlements.sort();
    let mut manifest = Map::new();
    manifest.insert("schemaVersion".into(), json!(2));
    manifest.insert("id".into(), json!(text("id")?));
    manifest.insert("name".into(), json!(text("name")?));
    manifest.insert("version".into(), json!(text("version")?));
    manifest.insert("runtime".into(), json!("lua-source"));
    manifest.insert(
        "runtimeVersion".into(),
        json!(text("runtimeVersion").unwrap_or_else(|_| "1".to_string())),
    );
    manifest.insert("entry".into(), json!(text("entry")?));
    manifest.insert(
        "minOsVersion".into(),
        json!(text("minFirmware").unwrap_or_else(|_| "2.0.0".to_string())),
    );
    if let Ok(icon) = text("icon") {
        manifest.insert("icon".into(), json!(icon));
    }
    manifest.insert("entitlements".into(), json!(entitlements));
    if let Some(budgets) = object.get("budgets").filter(|value| value.is_object()) {
        manifest.insert("budgets".into(), budgets.clone());
    }
    Ok(Value::Object(manifest))
}

fn copy_payload(source: &Path, target: &Path) -> Result<()> {
    fs::create_dir_all(target)
        .with_context(|| format!("creating runtime app work dir {}", target.display()))?;
    for entry in fs::read_dir(source)
        .with_context(|| format!("reading runtime app dir {}", source.display()))?
    {
        let entry = entry?;
        let name = entry.file_name();
        let name_text = name.to_string_lossy();
        // The v2 manifest replaces both authoring manifests; dot files are tooling state.
        if name_text.starts_with('.') || name_text == "manifest.json" || name_text == "manifest.txt"
        {
            continue;
        }
        let destination = target.join(&name);
        let kind = entry.file_type()?;
        if kind.is_dir() {
            copy_payload(&entry.path(), &destination)?;
        } else if kind.is_file() {
            fs::copy(entry.path(), &destination)
                .with_context(|| format!("copying {}", entry.path().display()))?;
        }
    }
    Ok(())
}

pub(crate) fn seed_boot_runtime_apps(
    apps: &[E2EBootRuntimeApp],
    sd_root: &Path,
    root_key: Option<&Path>,
) -> Result<()> {
    if apps.is_empty() {
        return Ok(());
    }
    let root_key = root_key.ok_or_else(|| {
        anyhow!(
            "boot.runtimeApps needs the trust-root key of the simulator test firmware: pass \
             --runtime-app-root-key or set {} (see apps/panda-os/tools/simulator-app-trust.sh)",
            ROOT_KEY_ENV
        )
    })?;
    let root = read_toolchain_fixture_key(root_key)?;
    let repo_root = repo_root_path()?;
    let toolchain_dir = repo_root.join("apps/toolchain");
    let path_arg = |path: &Path| -> Result<String> {
        path.to_str()
            .map(str::to_string)
            .ok_or_else(|| anyhow!("non-UTF8 runtime app path {}", path.display()))
    };
    for app in apps {
        let source = {
            let dir = Path::new(app.dir.trim());
            if dir.is_absolute() {
                dir.to_path_buf()
            } else {
                repo_root.join(dir)
            }
        };
        let authoring: Value = serde_json::from_str(
            &fs::read_to_string(source.join("manifest.json"))
                .with_context(|| format!("reading {}/manifest.json", source.display()))?,
        )
        .with_context(|| format!("parsing {}/manifest.json", source.display()))?;
        let manifest = schema_v2_manifest(&authoring)?;
        let app_id = manifest["id"].as_str().unwrap_or("app").to_string();

        let work: PathBuf = std::env::temp_dir().join(format!(
            "mofei-runtime-app-{}-{}",
            std::process::id(),
            stable_socket_suffix_hash(&format!("{}:{}", sd_root.display(), app_id))
        ));
        if work.exists() {
            fs::remove_dir_all(&work)?;
        }
        let app_dir = work.join("app");
        copy_payload(&source, &app_dir)?;
        fs::write(
            app_dir.join("manifest.json"),
            serde_json::to_vec_pretty(&manifest)?,
        )?;
        let package = work.join("app.pap");
        let signed = work.join("app.signed.pap");
        let dev_key = work.join("dev-key.json");
        let trust = [
            (
                "PANDA_INTERNAL_ROOT_PRIVATE_KEY_PEM",
                root.private_key_pem.as_str(),
            ),
            (
                "PANDA_TRUST_ROOT_PUBLIC_KEY_PEM",
                root.public_key_pem.as_str(),
            ),
        ];
        let toolchain = |args: &[&str], env: &[(&str, &str)]| {
            run_toolchain_fixture_command(&toolchain_dir, args, env)
        };
        toolchain(
            &["pack", &path_arg(&app_dir)?, "--out", &path_arg(&package)?],
            &[],
        )?;
        toolchain(
            &[
                "keygen",
                "--out",
                &path_arg(&dev_key)?,
                "--id",
                "dev.simulator.e2e",
            ],
            &[],
        )?;
        toolchain(
            &[
                "sign",
                &path_arg(&package)?,
                "--key",
                &path_arg(&dev_key)?,
                "--out",
                &path_arg(&signed)?,
            ],
            &trust,
        )?;
        toolchain(
            &[
                "install",
                &path_arg(&signed)?,
                "--target",
                &path_arg(sd_root)?,
            ],
            &trust,
        )?;
        fs::remove_dir_all(&work)
            .with_context(|| format!("removing runtime app work dir {}", work.display()))?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn official_buzzer_memory() -> Value {
        json!({
            "schemaVersion": 1,
            "id": "ai.pandacat.app.pandabuzzermemory",
            "name": "Buzzer Memory",
            "version": "1.0.0",
            "runtime": "lua-source",
            "runtimeVersion": "1",
            "entry": "src/main.lua",
            "icon": "assets/icon.pbm",
            "minFirmware": "2.0.0",
            "maxPackageBytes": 262144,
            "capabilities": {"storage": false, "network": false, "buzzer": true},
            "budgets": {"scriptHeapBytes": 262144, "maxRenderHz": 10}
        })
    }

    #[test]
    fn v2_manifest_keeps_budgets_and_declares_only_enabled_capabilities() {
        let manifest = schema_v2_manifest(&official_buzzer_memory()).unwrap();
        assert_eq!(manifest["schemaVersion"], 2);
        assert_eq!(manifest["entitlements"], json!(["buzzer"]));
        assert_eq!(manifest["budgets"]["scriptHeapBytes"], 262144);
        assert_eq!(manifest["budgets"]["maxRenderHz"], 10);
        assert_eq!(manifest["minOsVersion"], "2.0.0");
        assert_eq!(manifest["icon"], "assets/icon.pbm");
        assert!(manifest.get("maxPackageBytes").is_none());
        assert!(manifest.get("capabilities").is_none());
    }

    #[test]
    fn v2_manifest_without_capabilities_declares_no_entitlements() {
        let mut authoring = official_buzzer_memory();
        authoring.as_object_mut().unwrap().remove("capabilities");
        authoring.as_object_mut().unwrap().remove("budgets");
        authoring.as_object_mut().unwrap().remove("icon");
        let manifest = schema_v2_manifest(&authoring).unwrap();
        assert_eq!(manifest["entitlements"], json!([]));
        assert!(manifest.get("budgets").is_none());
        assert!(manifest.get("icon").is_none());
    }

    #[test]
    fn v2_manifest_passes_through_and_rejects_incomplete_input() {
        let v2 = json!({"schemaVersion": 2, "id": "x"});
        assert_eq!(schema_v2_manifest(&v2).unwrap(), v2);
        assert!(schema_v2_manifest(&json!({"schemaVersion": 1, "id": "a"})).is_err());
        assert!(schema_v2_manifest(&json!([])).is_err());
    }

    #[test]
    fn runtime_app_dirs_must_be_absolute_or_stay_inside_the_repo() {
        let app = |dir: &str| E2EBootRuntimeApp {
            dir: dir.to_string(),
        };
        assert!(validate_boot_runtime_apps(&[app("apps/x/y")]).is_ok());
        assert!(validate_boot_runtime_apps(&[app("/tmp/some-app")]).is_ok());
        assert!(validate_boot_runtime_apps(&[app("")]).is_err());
        assert!(validate_boot_runtime_apps(&[app("../escape")]).is_err());
        assert!(validate_boot_runtime_apps(&[app("apps/x"), app("apps/x")]).is_err());
    }

    #[test]
    fn seeding_without_a_root_key_names_the_missing_input() {
        let apps = [E2EBootRuntimeApp {
            dir: "apps/x".to_string(),
        }];
        let error = seed_boot_runtime_apps(&apps, Path::new("/nonexistent"), None).unwrap_err();
        assert!(error.to_string().contains("--runtime-app-root-key"));
        assert!(seed_boot_runtime_apps(&[], Path::new("/nonexistent"), None).is_ok());
    }

    #[cfg(feature = "product-fixtures")]
    #[test]
    fn official_app_installs_as_a_schema_v2_receipt() {
        let tmp =
            std::env::temp_dir().join(format!("mofei-runtime-app-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&tmp);
        fs::create_dir_all(&tmp).unwrap();
        let root_key = tmp.join("root-key.json");
        let toolchain = repo_root_path().unwrap().join("apps/toolchain");
        run_toolchain_fixture_command(
            &toolchain,
            &[
                "keygen",
                "--out",
                root_key.to_str().unwrap(),
                "--id",
                "root.simulator.e2e",
            ],
            &[],
        )
        .unwrap();
        let sd = tmp.join("sd");
        fs::create_dir_all(&sd).unwrap();
        let apps = [E2EBootRuntimeApp {
            dir: "apps/panda-developers/runtime-apps/official/panda-buzzer-memory".to_string(),
        }];
        seed_boot_runtime_apps(&apps, &sd, Some(&root_key)).unwrap();
        let id = "ai.pandacat.app.pandabuzzermemory";
        let receipt: Value = serde_json::from_str(
            &fs::read_to_string(sd.join(format!(".murphy/panda-apps/installed/{id}.json")))
                .unwrap(),
        )
        .unwrap();
        assert_eq!(receipt["appId"], id);
        assert_eq!(receipt["format"], 2);
        let installed: Value = serde_json::from_str(
            &fs::read_to_string(sd.join(format!("PandaApps/{id}/content/manifest.json"))).unwrap(),
        )
        .unwrap();
        assert_eq!(installed["entitlements"], json!(["buzzer"]));
        assert_eq!(installed["budgets"]["maxRenderHz"], 10);
        assert!(sd.join(format!("PandaApps/{id}/package.pap")).exists());
        fs::remove_dir_all(tmp).unwrap();
    }
}
