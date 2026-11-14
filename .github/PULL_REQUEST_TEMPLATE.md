## Summary

<!-- What changes, and why. One logical change per PR. -->

## Component

<!-- core / platform / host / android / docs / ci -->

## Testing

<!-- Commands run and their result. New or updated self-tests under tools/? -->

- [ ] `./build/iap2_test`, `./build/m3_test`, `./build/aa_wire_test` pass
- [ ] `ALLPLAY_CORE_ONLY=ON` build passes (if only `core/` changed)
- [ ] Android build verified locally (if `android/` changed)

## Documentation

- [ ] User-visible behavior documented in `README.md` and `README.zh-CN.md`
- [ ] `[Unreleased]` entry added to `CHANGELOG.md` and `CHANGELOG.zh-CN.md`
- [ ] Protocol behavior change recorded in `docs/PROTOCOL-NOTES.md` and `.zh-CN.md`

## Safety

- [ ] No credentials, `identity/` material, pairing records, or signing keys are included
- [ ] No platform headers or platform libraries added to `core/`

## Notes for reviewers

<!-- Anything that needs a decision, a hardware check, or a follow-up. -->
