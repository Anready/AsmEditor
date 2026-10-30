Baseline screenshots for the UI self-test (build.bat uitest).

A PNG here named like a screenshot of the test (01-editor.png, 02-debug.png, 03-flags.png, 04-profile.png)
is compared with the new screenshot when both have the same size; more than 2% of noticeably different pixels
fails the test. Take baselines from the "ui-test" artifact of the CI build (96 DPI): screenshots made on a
screen with another scale have other pixels.
