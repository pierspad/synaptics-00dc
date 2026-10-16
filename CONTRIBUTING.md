# Contributing

Include USB ID, distribution, kernel, TOD version, driver commit and the exact error.
Never attach raw captures, fingerprint images, fprintd templates or private pairing
credentials. Redact logs before sharing.

Run the offline commands in README. Protocol changes need parser/error tests;
transport changes need cancellation, deadline, short-transfer and cleanup tests.
Distinguish synthetic fixtures from hardware evidence. Keep changes focused and
describe which hardware paths remain untested.

Hardware experiments involving a person belong in the REA-FS experiment panel.
Do not change PAM or broaden Polkit privileges as a driver workaround.
