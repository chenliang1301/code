# yolov8_pose

## 目录

- [yolov8\_pose](#yolov8_pose)
  - [目录](#目录)
  - [1. 项目说明](#1-项目说明)
  - [2. 当前支持平台](#2-当前支持平台)
  - [3. 预训练模型](#3-预训练模型)
  - [4. 转换为 RKNN](#4-转换为-rknn)
  - [5. Python 示例](#5-python-示例)
  - [6. Android 示例](#6-android-示例)
      - [6.1 编译与构建](#61-编译与构建)
      - [6.2 推送示例文件到设备](#62-推送示例文件到设备)
      - [6.3 运行示例](#63-运行示例)
  - [7. Linux 示例](#7-linux-示例)
      - [7.1 编译与构建](#71-编译与构建)
      - [7.2 推送示例文件到设备](#72-推送示例文件到设备)
      - [7.3 运行示例](#73-运行示例)
  - [8. 预期结果](#8-预期结果)



## 1. 项目说明

本示例使用的模型来自以下开源项目：

https://github.com/airockchip/ultralytics_yolov8

yolov8n-pose PyTorch 模型下载链接：

[yolov8n-pose.pt](https://github.com/ultralytics/assets/releases/download/v8.2.0/yolov8n-pose.pt)<br />


## 2. 当前支持平台

RK3562, RK3566, RK3568, RK3576, RK3588, RV1126B


## 3. 预训练模型

下载链接：

[./yolov8n-pose.onnx](https://ftrg.zbox.filez.com/v2/delivery/data/95f00b0fc900458ba134f8b180b3f7a1/examples/yolov8_pose/yolov8n-pose.onnx)

使用以下命令下载：

```
cd model
./download_model.sh
```



## 4. 转换为 RKNN

*用法：*

```shell
cd python
python convert.py <onnx_model> <TARGET_PLATFORM> <dtype(optional)> <output_rknn_path(optional)>

# 示例：
python convert.py ../model/yolov8n-pose.onnx rk3588
# 输出模型保存为 ../model/yolov8n-pose.rknn
# 当前 C 语言示例仅支持 8 位量化
```

*参数说明：*

- `<onnx_model>`：ONNX 模型路径。
- `<TARGET_PLATFORM>`：NPU 平台名称，支持的平台见[这里](#2-当前支持平台)。
- `<dtype>(optional)`：可设为 `i8` 或 `fp`。`i8` 表示量化，`fp` 表示不量化，默认值为 `i8`。
- `<output_rknn_path>(optional)`：RKNN 模型保存路径。默认保存在 ONNX 模型同目录下，文件名为 `yolov8n-pose.rknn`。



## 5. Python 示例

*用法：*

```shell
cd python
# 使用 PyTorch 或 ONNX 模型推理
python yolov8_pose.py --model_path <pt_model/onnx_model>

# 使用 RKNN 模型推理
python yolov8_pose.py --model_path <rknn_model> --target <TARGET_PLATFORM>
```
*参数说明：*
- `<TARGET_PLATFORM>`：NPU 平台名称，例如 `rk3588`。

- `<pt_model / onnx_model / rknn_model>`：对应模型文件的路径。


## 6. Android 示例

#### 6.1 编译与构建

*用法：*

```sh
# 返回 rknn_model_zoo 根目录
cd ../../
export ANDROID_NDK_PATH=<android_ndk_path>

./build-android.sh -t <TARGET_PLATFORM> -a <ARCH> -d yolov8_pose

# 示例：
./build-android.sh -t rk3588 -a arm64-v8a -d yolov8_pose
```

*参数说明：*
- `<android_ndk_path>`：Android NDK 路径。
- `<TARGET_PLATFORM>`：NPU 平台名称，支持的平台见[这里](#2-当前支持平台)。
- `<ARCH>`：设备系统架构。可使用以下命令查询：
	```shell
	# 查询架构。Android 设备日志中应显示 arm64-v8a 或 armeabi-v7a。
	adb shell cat /proc/version
	```

#### 6.2 推送示例文件到设备

设备通过 USB 连接后，使用以下命令推送示例文件：

```shell
adb root
adb remount
adb push install/<TARGET_PLATFORM>_android_<ARCH>/rknn_yolov8_pose_demo/ /data/
```

#### 6.3 运行示例

```sh
adb shell
cd /data/rknn_yolov8_pose_demo

export LD_LIBRARY_PATH=./lib
./rknn_yolov8_pose_demo model/yolov8n-pose.rknn model/bus.jpg
```

- 运行后结果会保存为 `out.png`。可使用以下命令将结果拉取到主机查看：

  ```sh
  adb pull /data/rknn_yolov8_pose_demo/out.png
  ```



## 7. Linux 示例

#### 7.1 编译与构建

*用法：*

```shell
# 返回 rknn_model_zoo 根目录
cd ../../

# 如果构建时找不到 GCC_COMPILER，请设置其路径
(optional)export GCC_COMPILER=<GCC_COMPILER_PATH>

./build-linux.sh -t <TARGET_PLATFORM> -a <ARCH> -d yolov8_pose

# 示例：
./build-linux.sh -t rk3588 -a aarch64 -d yolov8_pose
```

构建命令中的 `-j` 是 `build-linux.sh` 的选项，表示禁用 libjpeg，以避免它与 OpenCV 的 libjpeg 冲突；它不是并行编译线程数。脚本实际使用 `make -j4` 编译。

*参数说明：*

- `<GCC_COMPILER_PATH>`：交叉编译器路径。
    ```sh
    export GCC_COMPILER=~/opt/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf
    ```
- `<TARGET_PLATFORM>`：NPU 平台名称，支持的平台见[这里](#2-当前支持平台)。
- `<ARCH>`：设备系统架构。可使用以下命令查询：
  
  ```shell
  # 查询架构。Linux 设备日志中应显示 aarch64 或 armhf。
  adb shell cat /proc/version
  ```

#### 7.2 推送示例文件到设备

- 设备通过 USB 连接时，使用以下命令推送示例文件：

```shell
adb push install/<TARGET_PLATFORM>_linux_<ARCH>/rknn_yolov8_pose_demo/ /userdata/
```

- 对于其他开发板，可使用 `scp` 等方式将 `install/<TARGET_PLATFORM>_linux_<ARCH>/rknn_yolov8_pose_demo/` 下的所有文件复制到设备的 `userdata` 目录。

#### 7.3 运行示例

```sh
adb shell
cd /userdata/rknn_yolov8_pose_demo

export LD_LIBRARY_PATH=./lib
./rknn_yolov8_pose_demo model/yolov8n-pose.rknn model/bus.jpg
```

运行姿态检测并启动网页实时预览（需要包含 `videoio` 和 `imgcodecs` 模块的 OpenCV）：

```sh
./rknn_yolov8_pose_demo ./model/yolov8_pose.rknn camera --camera 0 --hand right --web
# 使用自定义 HTTP 端口 8090；添加 --display 还会在设备本机全屏显示预览：
./rknn_yolov8_pose_demo ./model/yolov8_pose.rknn camera --camera 0 --hand right --display --web 8090
# ROI 可在网页右下角控制面板中配置（x、y、宽、高均使用画面百分比），应用后立即生效并重置当前跟踪。
```

在同一网络中的浏览器打开 `http://<RK3568-IP>:8080/`（如果使用 `--web` 指定端口，请替换为对应端口）。网页显示带标注的实时画面；点击右上角全屏按钮可进入或退出全屏。按 `Ctrl+C` 停止程序，并确保设备允许访问对应 TCP 端口。

网页右下角的 `⋯` 按钮可展开控制栏；控制栏默认收起，减少对画面的遮挡。文件名输入框会记住上次填写的名称。点击 `开始录像` 录制，点击 `停止录像` 后将带标注的预览保存到电脑。只有浏览器支持文件系统访问 API 且页面处于安全上下文时，`选择路径` 才能打开电脑上的保存位置选择器；通过 `http://<RK3568-IP>:8080/` 访问时，通常不具备该权限，按钮会提示这一点，视频将下载到浏览器默认下载目录。录像以 WebM 格式保存在电脑上，RK3568 不保存视频。已接受的挥拍事件（包括标定挥拍）仍会追加写入设备上的 `swing_events.csv`，包含会话 ID、事件编号、开始/峰值/结束时间、峰值帧号与分数，以及带有效性标记的指标值。

停止录制并等待浏览器完成保存后，可从选定目录（或浏览器的下载目录）打开 `.webm` 文件。可将文件拖入 Chrome、Edge 等支持 WebM 的浏览器播放，也可使用 VLC。网页会保持相机画面比例，超出浏览器视口的部分以黑边补齐；录像保留实时预览叠加信息，可暂停或拖动进度条逐帧查看。关闭网页前若未停止录制，未完成的录像可能丢失。

画面左上角叠加信息使用透明底色和抗锯齿描边，降低对画面的遮挡。字段说明如下：

- `ROI`：黄色挥拍区域。默认 x=10%、y=25%、宽 80%、高 70%；展开网页右下角控制面板后可输入百分比并点击 `Apply ROI`，应用后立即更新且重置当前跟踪。检测到的人体框中心需位于 ROI 内才参与分析。
- `State`：跟踪或触发状态，例如 `ready`、`rising`、`peak`、`falling`、`cooldown`，也可能显示拒绝或超时原因。
- `Swings`：程序启动后已接受的挥拍次数，包含标定挥拍。动作进行中显示的 `Current` 是当前候选事件编号。
- `Score`：当前手腕、肘部和肩部运动速度加权归一化后的动作分数。显示 `n/a` 表示尚无可用于计算的前序有效关键点。
- `Peak`：当前或最近一次完成的候选动作中观测到的最高分数。
- `THRESH`：挥拍触发阈值。初始使用备用阈值，标定后会更新；`calibrated` 表示当前使用的是已标定阈值。
- `Last`：最近一次接受的标定挥拍或分析挥拍及其峰值分数。录制时画面右上角会显示红色录制计时。

候选动作的分数回落到 `max(LOW_THRESH, 0.5 × Peak)` 时进入释放确认。候选若超过 2 秒仍未释放，终端会打印超时时长、峰值、触发阈值和释放阈值，便于区分动作持续时间过长与分数未充分回落。

录像中的左上角字段与实时预览相同：`ROI` 是黄色有效挥拍区域；`State` 是当前跟踪/触发状态；`Swings` 是已接受的事件数，`Current` 是正在分析的候选编号；`Score` 是当前动作分数；`Peak` 是当前或最近候选动作的最高分数；`THRESH` 是触发阈值；`Last` 是最近一次接受的事件。只有录制过程中，画面右上角才会显示红色 `REC` 计时。

- 运行后结果会保存为 `out.png`。可使用以下命令将结果拉取到主机查看：

  ```
  adb pull /userdata/rknn_yolov8_pose_demo/out.png
  ```



## 8. 预期结果

此示例会打印测试图像中的检测类别及对应分数，例如：
```
person @ (209 244 286 506) 0.884
person @ (478 238 559 526) 0.868
person @ (110 238 230 534) 0.825

```
<img src="python/result.jpg">

<br>
- 注意：不同平台以及不同版本的工具和驱动可能会产生略有差异的结果。
