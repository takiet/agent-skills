# YOLOv5 Object Detection ACAP

Develop an object detection application based on YOLOv5s model running on Axis devices. The application draws the bounding box and the label for each detected object.
This is a sample application not prodution ready and then keep it as simple as possible.

## Workflow

1. Use vdo to capture YUV/NV12 image
2. Use larod preprocessing to transform the NV12 image into RGB-interleaved, 640x640 with bottom padding image for the input to tensorflow lite
3. Use larod to run inference based on YOLOv5 model
4. Decode the output (w/ NMS)
5. Use axoverlay to draw the bounding boxes for the deteced object with their label.
6. Return to 1 and repeat the process the specified number of iterations.

## Requirements

- The capture frame rate must be specified (range: 1 ~ 10, default: 1 fps) via axparameter
  - Restart required to reflect the rate but not needed to automatically restart
- The number of loops must be specified (range: 1 ~ inifinite, default: 10) via axparameter
  - Restart required to reflect the number but not needed to automatically restart

## Verification

- [] Capture 16:9 (640x360) images and generate RGB-interleaved, 640x640 squared iamges (NOT stretched) as the output by preprocessing
- [] Run the object detection inference using a test image and the output must be close the one that is generated on host
- [] Overlay the several boxes with the specified locations and their label on the top-left of the box. (Let the user check if the boxes are drawn)
