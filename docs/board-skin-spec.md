# Official-reference board skins

Panda-owned simulator code is licensed under MIT. Official product images are
used only as implementation references; the repository ships CSS-rendered skins,
not copied product photography. Reference assets and third-party materials
retain their applicable rights and notices.

## M5Stack PaperS3

Reference: https://docs.m5stack.com/en/core/PaperS3

Official dimensions: 121.5 × 67.7 × 7.7 mm. The product uses a 4.7-inch
960 × 540 full-screen e-paper panel, one physical button, microSD expansion and
a hanging-ear structure. The simulator portrait view uses the same physical
proportion rotated 90 degrees: 67.7 / 121.5.

Rendered structure:
- off-white thin chassis and near-edge-to-edge screen,
- one right-side physical key,
- bottom-center hanging loop,
- bottom USB-C and microSD edge slots,
- M5/PaperS3 front branding treatment.

## LILYGO T5-4.7 E-Paper S3 Pro

Reference: https://wiki.lilygo.cc/products/t5-series/t5-e-paper-s3-pro/

Official dimensions: 129 × 69 × 11 mm. The product uses a 4.7-inch 960 × 540
ED047TC1 e-paper panel, GT911 touch, TF storage, USB-C, two QWIIC connectors,
two M4 mounting holes, and RST + BOOT + IO48 + PWR controls. The simulator
portrait view uses the physical proportion rotated 90 degrees: 69 / 129.

Rendered structure:
- light-silver front bezel with dark 11 mm edge treatment,
- circular lower front touch/power mark,
- four-control side rail in official logical order,
- USB-C and TF edge slots,
- LILYGO model mark and thicker industrial body.

## Scope

These skins reproduce public dimensions and visible structural cues. They are
not CAD, manufacturing drawings, trademark artwork, or a simulation of physical
e-paper reflectance. Framebuffer dimensions, touch mapping and QEMU peripheral
behavior remain controlled by the board registry.
