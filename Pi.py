import cv2
import time
import serial
import threading
from ultralytics import YOLO
from flask import Flask, Response
import paho.mqtt.client as mqtt

#  CAMERA 
CAM_URL = "http://10.102.183.100:81/stream"
FRAME_W, FRAME_H = 320, 240
RECONNECT_DELAY = 1.0

#  MODEL 
MODEL_PATH = "/home/pi/DA/model/run1/weights/best.pt"
TARGET_CLASS_NAME = "cup"

CONF_THRES = 0.7
IOU_THRES = 0.5
INFER_IMGSZ = 320
INFER_INTERVAL = 0.1
RESULT_MAX_AGE = 0.35
DISPLAY_BOX_MAX_AGE = 0.25
LOST_TIMEOUT = 0.6

#  STREAM 
ENABLE_STREAM = True
STREAM_HOST = "0.0.0.0"
STREAM_PORT = 5000
JPEG_QUALITY = 40
STREAM_FPS_LIMIT = 10

#  UART 
UART_PORT = "/dev/serial0"
UART_BAUD = 115200
UART_TIMEOUT = 0.1
UART_MIN_SEND_INTERVAL = 0.1

#  FILTER 
EMA_ALPHA = 0.7
DEAD_ZONE = 10
MAX_ERROR_STEP = 120

#  MQTT 
MODE = "Auto"
MQTT_BROKER = "10.102.183.12"
MQTT_PORT = 1883
mqtt_client = None

#  GLOBAL 
running = True

raw_frame = None
raw_frame_id = 0
raw_frame_time = 0.0
raw_lock = threading.Lock()

display_frame = None
display_lock = threading.Lock()

visual_result = {"box": None,"conf": 0.0,"error_x": 0,"time": 0.0,}
visual_lock = threading.Lock()

control_state = {"found": 0,"error_x": 0,"bbox_area": 0,"conf": 0.0,"seq": 0,}
control_lock = threading.Lock()

#  LOAD MODEL 
print("[INFO] Loading YOLO...")
model = YOLO(MODEL_PATH)
try:
    model.fuse()
except Exception:
    pass

class_names = model.names
target_id = None
for k, v in class_names.items():
    if str(v).lower() == TARGET_CLASS_NAME.lower():
        target_id = int(k)
        break

print("[INFO] Target ID:", target_id)
if target_id is None:
    print("[WARN] Target class not found. The model will use all classes.")

#  HELPER 
def now_s():
    return time.monotonic()

def get_mode():
    return MODE 

def update_control(found, error_x, bbox_area, conf, frame_id):
    with control_lock:
        control_state["found"] = int(found)
        control_state["error_x"] = int(error_x)
        control_state["bbox_area"] = int(bbox_area)
        control_state["conf"] = float(conf)
        control_state["seq"] += 1

def reset_control(frame_id=0):
    update_control(0, 0, 0, 0.0, frame_id)

def update_visual(box, conf, error_x, frame_id):
    with visual_lock:
        visual_result["box"] = box
        visual_result["conf"] = float(conf)
        visual_result["error_x"] = int(error_x)
        visual_result["time"] = now_s()

def clear_visual():
    update_visual(None, 0.0, 0, 0)

def build_uart_packet(state):
    return (
        f"$TRACK,{int(state['found'])},{int(state['error_x'])},"
        f"{int(state['bbox_area'])}*\n"
    ).encode()

def pick_best_box(results):
    best_box = None
    best_conf = 0.0
    best_score = -1.0

    if not results or len(results) == 0 or results[0].boxes is None:
        return None, 0.0

    for box in results[0].boxes:
        cls = int(box.cls.item())
        conf = float(box.conf.item())

        if target_id is not None and cls != target_id:
            continue

        x1, y1, x2, y2 = box.xyxy[0].tolist()
        area = max(0, x2 - x1) * max(0, y2 - y1)
        score = area * conf

        if score > best_score:
            best_score = score
            best_box = (x1, y1, x2, y2)
            best_conf = conf

    return best_box, best_conf

#  MQTT 
def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print("[INFO] MQTT connected")
        client.subscribe("control/mode")
    else:
        print("[WARN] MQTT connect failed:", rc)

def on_disconnect(client, userdata, rc):
    print("[WARN] MQTT disconnected")

def on_message(client, userdata, msg):
    global MODE
    if msg.topic == "control/mode":
        new_mode = msg.payload.decode().strip()
        if new_mode in ("Auto", "Manual"):
            MODE = new_mode
            if MODE != "Auto":
                reset_control()
                clear_visual()

def setup_mqtt():
    global mqtt_client
    mqtt_client = mqtt.Client()
    mqtt_client.on_connect = on_connect
    mqtt_client.on_disconnect = on_disconnect
    mqtt_client.on_message = on_message

    while running:
        try:
            mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)
            mqtt_client.loop_start()
            return
        except Exception as e:
            print("[WARN] MQTT reconnecting...", e)
            time.sleep(2)

#  STREAM 
app = Flask(__name__)

def generate_stream():
    delay = 1.0 / STREAM_FPS_LIMIT

    while running:
        with display_lock:
            frame = None if display_frame is None else display_frame.copy()

        if frame is None:
            time.sleep(0.01)
            continue

        ok, buffer = cv2.imencode(
            ".jpg",
            frame,
            [int(cv2.IMWRITE_JPEG_QUALITY), JPEG_QUALITY]
        )

        if ok:
            yield (
                b"--frame\r\nContent-Type: image/jpeg\r\n\r\n" +
                buffer.tobytes() +
                b"\r\n"
            )

        time.sleep(delay)

@app.route("/video")
def video():
    return Response(
        generate_stream(),
        mimetype="multipart/x-mixed-replace; boundary=frame"
    )

def run_flask():
    app.run(
        host=STREAM_HOST,
        port=STREAM_PORT,
        debug=False,
        use_reloader=False,
        threaded=True
    )

#  CAMERA 
def open_camera():
    camera = cv2.VideoCapture(CAM_URL)
    try:
        camera.set(cv2.CAP_PROP_BUFFERSIZE, 1)
    except Exception:
        pass
    return camera

def camera_reader():
    global raw_frame, raw_frame_id, raw_frame_time

    cap = open_camera()

    while running:
        if cap is None or not cap.isOpened():
            time.sleep(RECONNECT_DELAY)
            cap = open_camera()
            continue

        ret, frame = cap.read()
        if not ret:
            try:
                cap.release()
            except Exception:
                pass
            time.sleep(RECONNECT_DELAY)
            cap = open_camera()
            continue

        frame = cv2.resize(frame, (FRAME_W, FRAME_H))
        t = now_s()

        with raw_lock:
            raw_frame = frame
            raw_frame_id += 1
            raw_frame_time = t

#  DISPLAY 
def display_worker():
    global display_frame

    delay = 1.0 / STREAM_FPS_LIMIT

    while running:
        with raw_lock:
            frame = None if raw_frame is None else raw_frame.copy()

        if frame is None:
            time.sleep(0.01)
            continue

        if get_mode() == "Auto":
            with visual_lock:
                vr = visual_result.copy()

            if vr["box"] is not None and now_s() - vr["time"] <= DISPLAY_BOX_MAX_AGE:
                x1, y1, x2, y2 = vr["box"]
                conf = vr["conf"]
                err = vr["error_x"]

                cv2.rectangle(
                    frame,
                    (int(x1), int(y1)),
                    (int(x2), int(y2)),
                    (0, 255, 0),
                    2
                )
                label = f"{TARGET_CLASS_NAME}: {conf * 100:.1f}%"
                cv2.putText(
                    frame,
                    label,
                    (int(x1), max(int(y1) - 10, 20)),
                    cv2.FONT_HERSHEY_SIMPLEX,
                    0.6,
                    (0, 255, 0),
                    2
                )
                cv2.putText(
                    frame,
                    f"err:{err}",
                    (8, FRAME_H - 10),
                    cv2.FONT_HERSHEY_SIMPLEX,
                    0.45,
                    (255, 255, 255),
                    1
                )

        with display_lock:
            display_frame = frame

        time.sleep(delay)

#  INFERENCE + CONTROL 
def inference_worker():
    last_infer_time = 0.0
    last_infer_frame_id = -1
    last_detect_time = 0.0
    lost_sent = False

    ema_error = 0.0
    last_sent_error = 0

    while running:
        if get_mode() != "Auto":
            reset_control()
            clear_visual()
            lost_sent = True
            ema_error = 0.0
            last_sent_error = 0
            time.sleep(0.5)
            continue

        t_now = now_s()
        if t_now - last_infer_time < INFER_INTERVAL:
            time.sleep(0.002)
            continue

        with raw_lock:
            if raw_frame is None:
                frame = None
                frame_id = 0
                frame_time = 0.0
            else:
                frame = raw_frame.copy()
                frame_id = raw_frame_id
                frame_time = raw_frame_time

        if frame is None:
            time.sleep(0.01)
            continue

        if frame_id == last_infer_frame_id:
            time.sleep(0.002)
            continue

        last_infer_frame_id = frame_id
        last_infer_time = t_now

        if t_now - frame_time > RESULT_MAX_AGE:
            if not lost_sent:
                reset_control(frame_id)
                clear_visual()
                lost_sent = True
            continue

        try:
            yolo_config = dict(imgsz=INFER_IMGSZ,conf=CONF_THRES,iou=IOU_THRES,device="cpu",verbose=False,half=False
            )
            if target_id is not None:
                yolo_config["classes"] = [target_id]

            results = model.predict(frame, **yolo_config)
        except Exception as e:
            print("[ERROR] Inference error:", e)
            if not lost_sent:
                reset_control(frame_id)
                clear_visual()
                lost_sent = True
            time.sleep(0.03)
            continue

        infer_done_time = now_s()
        result_age = infer_done_time - frame_time

        if result_age > RESULT_MAX_AGE:
            if not lost_sent:
                reset_control(frame_id)
                clear_visual()
                lost_sent = True
            continue

        w = frame.shape[1]
        cx = w // 2
        best_box, best_conf = pick_best_box(results)

        if best_box is not None:
            x1, y1, x2, y2 = best_box
            mx = int((x1 + x2) / 2)
            bbox_w, bbox_h = int(x2 - x1), int(y2 - y1)
            bbox_area = bbox_w * bbox_h

            err_x = mx - cx
            if abs(err_x) < DEAD_ZONE:
                err_x = 0

            filtered_error = (1.0 - EMA_ALPHA) * ema_error + EMA_ALPHA * err_x

            delta = filtered_error - last_sent_error
            if delta > MAX_ERROR_STEP:
                filtered_error = last_sent_error + MAX_ERROR_STEP
            elif delta < -MAX_ERROR_STEP:
                filtered_error = last_sent_error - MAX_ERROR_STEP

            ema_error = filtered_error
            last_sent_error = int(filtered_error)

            last_detect_time = infer_done_time
            lost_sent = False

            update_control(1, last_sent_error, bbox_area, best_conf, frame_id)
            update_visual(best_box, best_conf, last_sent_error, frame_id)

        else:
            clear_visual()

            if infer_done_time - last_detect_time > LOST_TIMEOUT:
                ema_error = 0.0
                last_sent_error = 0
                if not lost_sent:
                    reset_control(frame_id)
                    lost_sent = True
            else:
                reset_control(frame_id)

#  UART 
def open_uart():
    try:
        ser = serial.Serial(UART_PORT, UART_BAUD, timeout=UART_TIMEOUT)
        time.sleep(0.5)
        return ser
    except Exception as e:
        print("[WARN] UART open failed:", e)
        return None

def uart_worker():
    ser = open_uart()
    last_sent_seq = -1
    last_send_time = 0.0

    while running:
        if ser is None or not ser.is_open:
            ser = open_uart()
            time.sleep(0.5)
            continue

        if get_mode() != "Auto":
            time.sleep(0.02)
            continue

        with control_lock:
            state = control_state.copy()

        if state["seq"] == last_sent_seq:
            time.sleep(0.002)
            continue

        t_now = now_s()
        if t_now - last_send_time < UART_MIN_SEND_INTERVAL:
            time.sleep(0.002)
            continue

        packet = build_uart_packet(state)

        try:
            ser.write(packet)
            last_sent_seq = state["seq"]
            last_send_time = t_now
        except Exception as e:
            print("[WARN] UART send failed:", e)
            try:
                ser.close()
            except Exception:
                pass
            ser = None

#  MAIN 
def main():
    setup_mqtt()

    if ENABLE_STREAM:
        threading.Thread(target=run_flask, daemon=True).start()
        threading.Thread(target=camera_reader, daemon=True).start()
        threading.Thread(target=display_worker, daemon=True).start()
        threading.Thread(target=inference_worker, daemon=True).start()
        threading.Thread(target=uart_worker, daemon=True).start()

    while running:
        time.sleep(1)

#  START 
if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        running = False
