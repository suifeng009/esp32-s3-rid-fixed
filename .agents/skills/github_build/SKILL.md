---
name: github_build
description: Automatically use GitHub Actions for compiling and releasing ESP32 firmware when asked to build or compile.
---

# GitHub Build Skill

When the user requests to "compile", "build the firmware", or asks about "compiling the project", you MUST prioritize the GitHub Actions cloud build pipeline instead of attempting local compilation (which may fail due to missing local tools).

Follow these steps:

1. **Commit and Push**:
   - Run `git status` to see what has changed.
   - Use `git add .` (or specific files) to stage changes.
   - Use `git commit -m "your description"` to commit.
   - Use `git push` to push to the `main` branch on the remote repository.
   
2. **Explain the GitHub Actions Pipeline**:
   - Remind the user that the project uses GitHub Actions (configured in `.github/workflows/build_and_release.yml`).
   - Pushing the code automatically triggers the `ESP-IDF Build` workflow in the cloud.

3. **Guide the User**:
   - Provide clear instructions on how to retrieve the firmware:
     1. Go to the GitHub repository page in the browser.
     2. Click on the **Actions** tab.
     3. Select the latest workflow run.
     4. Scroll to the bottom to the **Artifacts** section and download `esp32s3-rid-firmware.zip`.
     5. Extract it to get the `esp32-s3-rid-combined.bin` file ready for flashing.
