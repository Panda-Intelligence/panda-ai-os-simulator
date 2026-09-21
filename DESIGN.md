# Panda Simulator host design contract

## 0. Design Read

This is a focused device-lab workspace for Panda owners and firmware or simulator developers. It has an independent
visual identity rather than reproducing a VS Code shell or either research reference.

**Dials: ENERGY 2 / RHYTHM 3 / MOTION 1.**

- **Energy 2:** a restrained signal accent identifies actions and live state without turning the tool into a marketing
  surface.
- **Rhythm 3:** toolbar, board context, device stage, serial output, and inspector use different compositions because
  each supports a different simulator job.
- **Motion 1:** transitions communicate state changes only. There are no decorative loops, parallax effects, or
  scroll choreography.

## 1. Identity and palette

The host uses warm graphite in dark mode and paper-like stone in light mode, with a single copper-orange Panda signal
accent. This separates the operational host chrome from the simulated paper display while keeping status legible.

Typography uses the existing Panda IDE system tokens: the UI sans scale keeps controls compact and readable, while the
existing monospace token is reserved for paths and serial output because those values are operational text.

The accent is reserved for primary launch, selected board state, focus rings, and active drop feedback. It is not used
as a decorative stripe or a universal icon color. Surfaces use two elevations, flat workspace and raised controls, so
the device stage remains the focal point.

## 2. Composition and primitives

- The top bar groups device, firmware, locale, theme, and runtime actions because these are session-wide controls.
- The left rail and board region expose device selection and session context; they collapse through visible controls.
- The center stage gives the real `PanelCanvas` the largest visual weight. Its physical shell and paper framebuffer are
  deliberately outside host theme selectors.
- The serial panel is a named output surface with truthful empty and populated states.
- The inspector groups display, physical keys, and browser SD operations because they modify or inspect the current
  simulated session.
- Controls retain native semantics and visible focus rings. Existing selectors and probes remain stable.

## 3. Responsive states

- **Desktop:** board context, stage, serial output, and inspector coexist when their content fits.
- **Tablet:** the board region can be toggled independently and the inspector remains reachable as a scrollable panel;
  toolbar labels reduce before controls disappear.
- **Narrow:** the compact header keeps the primary runtime action and theme/locale controls visible. Board context,
  serial output, and inspector are separately disclosed with labeled buttons, then stack below the device stage.

Breakpoints are content-driven. Flex and grid children may shrink, wrap, or stack; no essential region is clipped or
removed without a keyboard-operable reveal path.

## 4. Accessibility and states

- Light, Dark, and Follow System are all visible in a segmented native radio group and localized through the existing
  simulator i18n path. The three-way control keeps the preference visible without hiding it behind a menu.
- The GitHub link is a real external navigation to the standalone simulator repository and remains text-labeled for
  keyboard and screen-reader users.
- The resolved host theme sets `data-theme` and `color-scheme`; the simulated framebuffer remains paper-white.
- Normal text uses WCAG AA-safe ink/muted pairings in both palettes. Focus-visible outlines use the accent against the
  active surface and are never removed without a replacement.
- Status, unavailable runtime, empty SD, loading SD, disabled controls, and error output retain text labels rather than
  relying on color alone.
- `prefers-reduced-motion: reduce` removes transitions. The only normal motion is the short dropzone/state feedback
  transition, which explains the changed state to the user.

## 5. Reference use and accepted debt

Beautiful UI supplied the persistent workspace/navigation anatomy and named task states. beUI supplied the accessible
disclosure contract (`aria-expanded`, labeled trigger, and dismissal behavior). No reference palette, typography,
branding, sample data, page structure, dependency, or marketing copy is used.

The simulator continues to use the existing long `SimulatorDevicePane` because runtime ownership is intentionally kept
together for this redesign. The theme module is the only new behavioral seam; further presentational extraction is
deferred until a real second consumer exists.
