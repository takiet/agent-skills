# CLAUDE.md

This project implements performance test program between cpu-proc (libyuv) and a9-gpu-proc (opencl) for preprocessing: crop, scale, and convert.

## Test Scenarios

One single binary executes the following 4 scenarios at once.

### Crop
1. Capture 1920x1080 image with NV12 format
2. Repeat cropping 300x300 portion with the same format 50 times
  - The location varies on each cropping but the same sequence both for cpu-proc and a9-gpu-proc
3. Calculate and output the mean and standard deviation of the computation time to syslog

### Scale
1. Capture 1920x1080 image with NV12 format
2. Repeat scaling to 300x300 image size with the same format 50 times
3. Calculate and output the mean and standard deviation of the computation time to syslog

### Convert to interleaved RGB
1. Capture 1920x1080 image with NV12 format
2. Repeat converting to interleaved RGB with the same size 50 times
3. Calculate and output the mean and standard deviation of the computation time to syslog

### Convert to planar RGB
1. Capture 1920x1080 image with NV12 format
2. Repeat converting to planar RGB with the same size 50 times
3. Calculate and output the mean and standard deviation of the computation time to syslog


## Verification

- [] Verify the preprocessing correctly work before moving forward the next process by asking the user to check both original and processed images.

