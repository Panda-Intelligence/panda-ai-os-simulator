use super::*;
use std::io::Write;

struct BatchFixture {
    root: PathBuf,
    sd_root: PathBuf,
    artifacts: PathBuf,
    image: PathBuf,
}

impl Drop for BatchFixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.root);
    }
}

impl BatchFixture {
    fn new(name: &str) -> Self {
        let root = std::env::temp_dir().join(format!(
            "panda-batch-resource-{name}-{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&root);
        let sd_root = root.join("batch.sd/case");
        let artifacts = root.join("artifacts/case");
        let image = artifacts.join("sdcard.img");
        fs::create_dir_all(sd_root.join(".murphy/fonts")).unwrap();
        fs::create_dir_all(&artifacts).unwrap();
        fs::write(sd_root.join(".murphy/fonts/fixed.mfp"), vec![0x57; 8192]).unwrap();
        fs::write(sd_root.join("settings.json"), b"seed state").unwrap();
        sd_image::create_sd_image(&sd_root, &image, 64 * 1024 * 1024).unwrap();
        {
            let image_file = fs::OpenOptions::new()
                .read(true)
                .write(true)
                .open(&image)
                .unwrap();
            let fat = fatfs::FileSystem::new(image_file, fatfs::FsOptions::new()).unwrap();
            let root_dir = fat.root_dir();
            let mut settings = root_dir.open_file("settings.json").unwrap();
            settings.truncate().unwrap();
            settings.write_all(b"guest state").unwrap();
            let mut marker = root_dir.create_file("persisted.txt").unwrap();
            marker.write_all(b"guest-only marker").unwrap();
        }
        Self {
            root,
            sd_root,
            artifacts,
            image,
        }
    }

    fn persist(&self, status: &str) -> CaseRunOutcome {
        let result = CaseResult {
            case_id: "case".to_string(),
            status: status.to_string(),
            error: (status == "FAIL").then(|| "assertion failed".to_string()),
            steps: vec![StepResult {
                step_type: "assert_activity".to_string(),
                status: status.to_string(),
                elapsed_ms: 12,
                error: None,
            }],
            total_elapsed_ms: 25,
            screenshot: self
                .artifacts
                .join("screenshot.png")
                .to_string_lossy()
                .into_owned(),
            transcript: self
                .artifacts
                .join("transcript.log")
                .to_string_lossy()
                .into_owned(),
            firmware: "retained-firmware.bin".to_string(),
        };
        let geometry = BoardGeometry {
            raw_width: 8,
            raw_height: 8,
            framebuffer_width: 8,
            framebuffer_height: 8,
            framebuffer_format: FramebufferFormat::Mono1,
            output_width: 8,
            output_height: 8,
        };
        persist_e2e_artifacts(
            &self.artifacts,
            &result,
            &["actual persisted transcript".to_string()],
            &[0xff; 8],
            &geometry,
        )
        .unwrap();
        fs::write(
            self.artifacts.join("runner.log"),
            b"retained runner diagnostics",
        )
        .unwrap();
        fs::create_dir_all(self.artifacts.join("assertions")).unwrap();
        fs::copy(
            self.artifacts.join("screenshot.png"),
            self.artifacts.join("assertions/step-1.png"),
        )
        .unwrap();
        fs::write(
            self.artifacts.join("assertions/step-1.json"),
            b"{\"activity\":\"Reader\"}",
        )
        .unwrap();
        let mut flash = fs::File::create(self.artifacts.join("flash.img")).unwrap();
        flash.write_all(b"forensic flash bytes").unwrap();
        flash.set_len(16 * 1024 * 1024).unwrap();
        CaseRunOutcome {
            case_id: result.case_id,
            status: result.status,
            artifacts_dir: self.artifacts.clone(),
            error: result.error,
        }
    }

    fn proof_snapshot(&self) -> Vec<(PathBuf, Vec<u8>)> {
        [
            "result.json",
            "transcript.log",
            "screenshot.png",
            "runner.log",
            "assertions/step-1.png",
            "assertions/step-1.json",
        ]
        .into_iter()
        .map(|path| {
            (
                PathBuf::from(path),
                fs::read(self.artifacts.join(path)).unwrap(),
            )
        })
        .collect()
    }

    fn assert_proofs(&self, before: &[(PathBuf, Vec<u8>)]) {
        for (path, bytes) in before {
            assert_eq!(
                &fs::read(self.artifacts.join(path)).unwrap(),
                bytes,
                "proof changed: {}",
                path.display()
            );
        }
    }

    fn assert_images_retained(&self, image_before: &[u8], flash_before: &[u8]) {
        assert_eq!(fs::read(&self.image).unwrap(), image_before);
        assert_eq!(
            fs::read(self.artifacts.join("flash.img")).unwrap(),
            flash_before
        );
        assert_eq!(
            fs::read(self.sd_root.join("settings.json")).unwrap(),
            b"seed state"
        );
    }
}

#[test]
fn generated_batch_scratch_without_sd_assertions_does_not_export_fat() {
    let fixture = BatchFixture::new("no-export");
    fs::remove_file(fixture.sd_root.join(".murphy/fonts/fixed.mfp")).unwrap();
    sync_e2e_sd_after_case(
        &fixture.image,
        &fixture.sd_root,
        E2eSdRootPolicy::BatchScratch,
        &[],
    )
    .unwrap();
    assert_eq!(
        fs::read(fixture.sd_root.join("settings.json")).unwrap(),
        b"seed state"
    );
    assert!(!fixture.sd_root.join("persisted.txt").exists());
    assert!(!fixture.sd_root.join(".murphy/fonts/fixed.mfp").exists());
    let image = fs::OpenOptions::new()
        .read(true)
        .write(true)
        .open(&fixture.image)
        .unwrap();
    let fat = fatfs::FileSystem::new(image, fatfs::FsOptions::new()).unwrap();
    assert!(fat.root_dir().open_file("persisted.txt").is_ok());
}

#[test]
fn retained_caller_or_single_case_sd_exports_actual_guest_writes() {
    let fixture = BatchFixture::new("retained-export");
    sync_e2e_sd_after_case(
        &fixture.image,
        &fixture.sd_root,
        E2eSdRootPolicy::Retain,
        &[],
    )
    .unwrap();
    assert_eq!(
        fs::read(fixture.sd_root.join("settings.json")).unwrap(),
        b"guest state"
    );
    assert_eq!(
        fs::read(fixture.sd_root.join("persisted.txt")).unwrap(),
        b"guest-only marker"
    );
    assert_eq!(
        fs::read(fixture.sd_root.join(".murphy/fonts/fixed.mfp")).unwrap(),
        vec![0x57; 8192]
    );
}

#[test]
fn generated_batch_scratch_exports_real_fat_for_deferred_sd_assertions() {
    let fixture = BatchFixture::new("deferred-export");
    let assertions = [DeferredSdFileAssertion {
        result_index: 0,
        step_type: "assert_sd_file_contains".to_string(),
        relative_path: PathBuf::from("persisted.txt"),
        display_path: "/persisted.txt".to_string(),
        contains: Some("guest-only marker".to_string()),
        step_start: StdInstant::now(),
    }];
    assert!(!fixture.sd_root.join("persisted.txt").exists());
    sync_e2e_sd_after_case(
        &fixture.image,
        &fixture.sd_root,
        E2eSdRootPolicy::BatchScratch,
        &assertions,
    )
    .unwrap();
    assert_sd_file_contains(
        &fixture.sd_root,
        &assertions[0].relative_path,
        &assertions[0].display_path,
        assertions[0].contains.as_deref().unwrap(),
    )
    .unwrap();
    assert_eq!(
        fs::read(fixture.sd_root.join("settings.json")).unwrap(),
        b"guest state"
    );
}

#[test]
fn completed_pass_batch_discards_only_owned_images_and_preserves_all_proofs() {
    let fixture = BatchFixture::new("pass-cleanup");
    let outcome = Ok(fixture.persist("PASS"));
    let proofs = fixture.proof_snapshot();
    cleanup_e2e_batch_artifacts(Some(&fixture.sd_root), &fixture.artifacts, &outcome).unwrap();
    assert!(!fixture.sd_root.exists());
    assert!(!fixture.image.exists());
    assert!(!fixture.artifacts.join("flash.img").exists());
    fixture.assert_proofs(&proofs);
    cleanup_e2e_batch_artifacts(Some(&fixture.sd_root), &fixture.artifacts, &outcome).unwrap();
    fixture.assert_proofs(&proofs);
}

#[test]
fn failed_batch_preserves_real_sd_flash_and_authoritative_proofs() {
    let fixture = BatchFixture::new("fail-retain");
    let outcome = Ok(fixture.persist("FAIL"));
    let proofs = fixture.proof_snapshot();
    let image = fs::read(&fixture.image).unwrap();
    let flash = fs::read(fixture.artifacts.join("flash.img")).unwrap();
    cleanup_e2e_batch_artifacts(Some(&fixture.sd_root), &fixture.artifacts, &outcome).unwrap();
    fixture.assert_images_retained(&image, &flash);
    fixture.assert_proofs(&proofs);
}

#[test]
fn errored_batch_preserves_real_sd_flash_and_partial_proofs() {
    let fixture = BatchFixture::new("error-retain");
    fixture.persist("FAIL");
    fs::remove_file(fixture.artifacts.join("result.json")).unwrap();
    let image = fs::read(&fixture.image).unwrap();
    let flash = fs::read(fixture.artifacts.join("flash.img")).unwrap();
    let transcript = fs::read(fixture.artifacts.join("transcript.log")).unwrap();
    let outcome = Err(anyhow!("writing result failed"));
    cleanup_e2e_batch_artifacts(Some(&fixture.sd_root), &fixture.artifacts, &outcome).unwrap();
    fixture.assert_images_retained(&image, &flash);
    assert_eq!(
        fs::read(fixture.artifacts.join("transcript.log")).unwrap(),
        transcript
    );
}

#[test]
fn pass_batch_cleanup_retains_caller_sd_and_raw_sd_image() {
    let fixture = BatchFixture::new("caller-retain");
    let outcome = Ok(fixture.persist("PASS"));
    let proofs = fixture.proof_snapshot();
    let image = fs::read(&fixture.image).unwrap();
    cleanup_e2e_batch_artifacts(None, &fixture.artifacts, &outcome).unwrap();
    assert_eq!(fs::read(&fixture.image).unwrap(), image);
    assert_eq!(
        fs::read(fixture.sd_root.join("settings.json")).unwrap(),
        b"seed state"
    );
    assert!(!fixture.artifacts.join("flash.img").exists());
    fixture.assert_proofs(&proofs);
}
