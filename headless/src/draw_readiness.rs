use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;

#[derive(Clone, Debug)]
struct InputBoundary {
    generation: u64,
    log_after: usize,
}

#[derive(Clone, Debug)]
struct ActivityDraw {
    generation: u64,
    log_index: usize,
    activity: String,
}

#[derive(Clone, Debug)]
struct CompletedPresent {
    generation: u64,
    log_index: usize,
}

#[derive(Clone, Debug, Default)]
pub(crate) struct Gray4DrawTracker {
    frame_generation: Arc<AtomicU64>,
    sequence: u64,
    boundary: Option<InputBoundary>,
    activity: Option<ActivityDraw>,
    completed: Option<CompletedPresent>,
}

impl Gray4DrawTracker {
    pub(crate) fn frame_generation(&self) -> Arc<AtomicU64> {
        self.frame_generation.clone()
    }

    fn next_generation(&mut self) -> u64 {
        self.sequence = self.sequence.wrapping_add(1).max(1);
        self.sequence
    }

    pub(crate) fn begin_input(&mut self, log_after: usize) {
        let generation = self.next_generation();
        self.boundary = Some(InputBoundary {
            generation,
            log_after,
        });
        self.activity = None;
        self.frame_generation.store(generation, Ordering::Release);
    }

    pub(crate) fn has_input_boundary(&self) -> bool {
        self.boundary.is_some()
    }

    pub(crate) fn observe_activity(&mut self, log_index: usize, activity: &str) {
        let Some(boundary) = &self.boundary else {
            return;
        };
        if log_index < boundary.log_after
            || self
                .activity
                .as_ref()
                .is_some_and(|draw| draw.activity.eq_ignore_ascii_case(activity))
        {
            return;
        }
        let generation = boundary.generation;
        self.activity = Some(ActivityDraw {
            generation,
            log_index,
            activity: activity.to_string(),
        });
    }

    pub(crate) fn record_line(&mut self, log_index: usize, line: &str) {
        let Some(activity) = completed_gray4_activity(line) else {
            return;
        };
        let (Some(boundary), Some(draw)) = (&self.boundary, &self.activity) else {
            return;
        };
        if log_index < boundary.log_after || log_index < draw.log_index {
            return;
        }
        self.completed = draw
            .activity
            .eq_ignore_ascii_case(activity)
            .then_some(CompletedPresent {
                generation: draw.generation,
                log_index,
            });
    }

    pub(crate) fn has_completed_present(&self, activity: &str) -> bool {
        let (Some(boundary), Some(draw), Some(completed)) =
            (&self.boundary, &self.activity, &self.completed)
        else {
            return false;
        };
        draw.generation == boundary.generation
            && completed.generation == draw.generation
            && completed.log_index >= draw.log_index
            && draw.log_index >= boundary.log_after
            && draw.activity.eq_ignore_ascii_case(activity.trim())
    }

    // contract: Host input generations are not guest frame IDs; use these pixels only after the IPC fence.
    pub(crate) fn ready(&self, frame_generation: u64, gray16: bool, activity: &str) -> bool {
        gray16
            && self.has_completed_present(activity)
            && self
                .boundary
                .as_ref()
                .is_some_and(|boundary| frame_generation == boundary.generation)
    }
}

fn completed_gray4_activity(line: &str) -> Option<&str> {
    let mut fields = line.split_whitespace();
    fields.find(|field| *field == "SIMGRAY4:")?;
    let mut event = None;
    let mut phase = None;
    let mut activity = None;
    for field in fields {
        let Some((key, value)) = field.split_once('=') else {
            continue;
        };
        let slot = match key {
            "event" => &mut event,
            "phase" => &mut phase,
            "activity" => &mut activity,
            _ => continue,
        };
        if slot.replace(value).is_some() {
            return None;
        }
    }
    if event != Some("present") || !matches!(phase, Some("direct" | "idle" | "static" | "lock")) {
        return None;
    }
    activity.filter(|name| !name.is_empty())
}

#[cfg(test)]
mod tests {
    use super::*;

    const READER: &str = "[I] SIMGRAY4: event=present phase=direct activity=Reader";

    fn reader_input(tracker: &mut Gray4DrawTracker, log_after: usize) -> u64 {
        tracker.begin_input(log_after);
        tracker.observe_activity(log_after, "Reader");
        tracker.frame_generation().load(Ordering::Acquire)
    }

    #[test]
    fn gray4_draw_requires_an_input_and_post_input_activity() {
        let mut tracker = Gray4DrawTracker::default();
        tracker.record_line(0, READER);
        assert!(!tracker.ready(0, true, "reader"));
        tracker.begin_input(1);
        tracker.record_line(1, READER);
        assert!(!tracker.ready(0, true, "reader"));
        tracker.observe_activity(2, "Reader");
        let generation = tracker.frame_generation().load(Ordering::Acquire);
        assert!(!tracker.ready(generation, true, "reader"));
    }

    #[test]
    fn gray4_draw_keeps_completion_between_input_and_assertion() {
        let mut tracker = Gray4DrawTracker::default();
        let generation = reader_input(&mut tracker, 8);
        tracker.record_line(9, READER);
        assert!(tracker.ready(generation, true, "reader"));
        assert!(tracker.ready(generation, true, "Reader"));
    }

    #[test]
    fn gray4_draw_rejects_old_input_frames_and_completion_before_reader() {
        let mut tracker = Gray4DrawTracker::default();
        let old_generation = reader_input(&mut tracker, 0);
        tracker.record_line(2, READER);
        tracker.begin_input(3);
        let before_activity = tracker.frame_generation().load(Ordering::Acquire);
        tracker.record_line(4, READER);
        tracker.observe_activity(5, "Reader");
        let generation = tracker.frame_generation().load(Ordering::Acquire);
        assert!(!tracker.ready(generation, true, "reader"));
        tracker.record_line(6, READER);
        assert!(!tracker.ready(old_generation, true, "reader"));
        assert!(tracker.ready(before_activity, true, "reader"));
        assert!(tracker.ready(generation, true, "reader"));
    }

    #[test]
    fn gray4_draw_rejects_an_old_activity_or_completion_offset() {
        let mut tracker = Gray4DrawTracker::default();
        tracker.begin_input(10);
        tracker.observe_activity(9, "Reader");
        assert!(!tracker.ready(
            tracker.frame_generation().load(Ordering::Acquire),
            true,
            "reader"
        ));
        tracker.observe_activity(10, "Reader");
        let generation = tracker.frame_generation().load(Ordering::Acquire);
        tracker.record_line(9, READER);
        assert!(!tracker.ready(generation, true, "reader"));
        tracker.record_line(11, READER);
        assert!(tracker.ready(generation, true, "reader"));
    }

    #[test]
    fn gray4_draw_rejects_mono_or_other_activity_frames() {
        let mut tracker = Gray4DrawTracker::default();
        tracker.begin_input(0);
        tracker.observe_activity(0, "FileBrowser");
        let library_generation = tracker.frame_generation().load(Ordering::Acquire);
        tracker.begin_input(1);
        tracker.observe_activity(1, "Reader");
        let generation = tracker.frame_generation().load(Ordering::Acquire);
        tracker.record_line(2, READER);
        assert!(!tracker.ready(library_generation, true, "reader"));
        assert!(!tracker.ready(generation, false, "reader"));
        assert!(!tracker.ready(generation, true, "filebrowser"));
        tracker.record_line(3, "SIMGRAY4: event=present phase=idle activity=Dashboard");
        assert!(!tracker.ready(generation, true, "reader"));
    }

    #[test]
    fn gray4_draw_does_not_promote_activity_or_failed_present_logs() {
        for line in [
            "UIREFRESH activity=Reader mode=fast",
            "SIMGRAY4: event=refused phase=direct activity=Reader",
            "SIMGRAY4: event=present phase=queued activity=Reader",
            "SIMGRAY4: event=present phase=direct activity=",
            "NOTSIMGRAY4: event=present phase=direct activity=Reader",
            "SIMGRAY4: event=present phase=direct activity=Reader activity=Dashboard",
        ] {
            let mut tracker = Gray4DrawTracker::default();
            let generation = reader_input(&mut tracker, 0);
            tracker.record_line(1, line);
            assert!(!tracker.ready(generation, true, "reader"), "{line}");
        }
    }

    #[test]
    fn gray4_draw_accepts_either_frame_completion_order() {
        let mut tracker = Gray4DrawTracker::default();
        let frame_first = reader_input(&mut tracker, 0);
        assert!(!tracker.ready(frame_first, true, "reader"));
        tracker.record_line(1, READER);
        assert!(tracker.ready(frame_first, true, "reader"));
        reader_input(&mut tracker, 2);
        tracker.record_line(3, READER);
        let completion_first = tracker.frame_generation().load(Ordering::Acquire);
        assert!(tracker.ready(completion_first, true, "reader"));
    }

    #[test]
    fn gray4_draw_retains_a_frame_header_before_activity_reaches_the_host() {
        let mut tracker = Gray4DrawTracker::default();
        tracker.begin_input(0);
        let frame_generation = tracker.frame_generation().load(Ordering::Acquire);
        tracker.observe_activity(0, "Reader");
        tracker.record_line(1, READER);
        assert!(tracker.ready(frame_generation, true, "reader"));
    }

    #[test]
    fn gray4_draw_does_not_reset_for_a_repeat_activity_before_completion() {
        let mut tracker = Gray4DrawTracker::default();
        let generation = reader_input(&mut tracker, 0);
        tracker.observe_activity(1, "Reader");
        tracker.record_line(2, READER);
        assert!(tracker.ready(generation, true, "reader"));
        tracker.observe_activity(3, "FileBrowser");
        assert!(!tracker.ready(generation, true, "reader"));
    }
}
