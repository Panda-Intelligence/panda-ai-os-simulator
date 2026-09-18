# Independent upstream boundary

Runtime sources, board geometry/input capabilities, UI, headless and build
tools live here. Murphy owns its firmware build, product/board mappings, golden
SD selection, signed release content and product E2E journeys.

Runtime, guest bundle and public site release are distinct identities. Missing
guest files must show an explicit unavailable state, never a fabricated boot.
Only an explicit integration configuration may point at a product repository.
Do not infer a parent checkout or require credentials to build this frontend.

The staging import is not permission to deploy its binaries or republish ROM.
Keep the existing Murphy consumer on develop until complete G2 integration
passes; the extraction integration is submitted as a draft PR.
