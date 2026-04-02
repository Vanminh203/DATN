/*  CONFIG  */

const ESP32_URL = "http://192.168.207.100";
const PI_STREAM = "http://192.168.207.208:5000/video";

const MQTT_BROKER = "ws://192.168.207.12:9001";

let mode = "Auto";
let flashlight = false;

/*  MQTT TOPICS  */

const CMD_TOPIC         = "control/move";
const MODE_TOPIC        = "control/mode";
const UI_TOPIC          = "ui/state";
const SPEED_TOPIC       = "control/speed";
const ROBOT_STATE_TOPIC = "robot/state";

/*  DOM  */

const camera = document.getElementById("camera");
const cameraOverlay = document.getElementById("cameraOverlay");
const joyStatus = document.getElementById("joyStatus");
const modeLabel = document.getElementById("modeLabel");
const flashBtn = document.getElementById("flashBtn");

const speedSlider = document.getElementById("speedSlider");
const speedValue = document.getElementById("speedValue");
const actualSpeedValue = document.getElementById("actualSpeedValue");

const distanceValue = document.getElementById("distanceValue");
const moveValue = document.getElementById("moveValue");

/*  LOCAL STATE  */

let isDraggingSpeed = false;
let speedPublishTimer = null;
let pendingSpeedValue = null;

/*  STREAM  */

function startStream() {
    if (!camera || !cameraOverlay) return;

    camera.hidden = false;
    camera.src = PI_STREAM;

    camera.onload = () => {
        cameraOverlay.style.display = "none";
    };

    camera.onerror = () => {
        console.log("Camera stream error");
        cameraOverlay.style.display = "flex";
    };
}

function stopStream() {
    if (!camera || !cameraOverlay) return;

    camera.src = "";
    camera.hidden = true;
    cameraOverlay.style.display = "flex";
}

/*  MQTT  */

const client = mqtt.connect(MQTT_BROKER);

client.on("connect", () => {
    console.log("MQTT Connected");

    client.subscribe(UI_TOPIC);
    client.subscribe(SPEED_TOPIC);
    client.subscribe(ROBOT_STATE_TOPIC);
});

client.on("reconnect", () => {
    console.log("MQTT Reconnecting...");
});

client.on("offline", () => {
    console.log("MQTT Offline");
});

client.on("error", (err) => {
    console.log("MQTT Error:", err);
});

client.on("message", (topic, msg) => {
    if (topic === UI_TOPIC) {
        let data = {};
        try {
            data = JSON.parse(msg.toString());
        } catch (e) {
            console.log("UI_TOPIC JSON parse error", e);
            return;
        }

        if (data.mode !== undefined) {
            mode = data.mode;
            updateModeUI();
        }

        if (data.flash !== undefined) {
            flashlight = data.flash;
            updateFlashUI();
        }

        if (data.joy !== undefined && joyStatus) {
            joyStatus.innerText = data.joy;
        }
    }

    if (topic === SPEED_TOPIC) {
        const speed = parseInt(msg.toString(), 10);

        if (!isNaN(speed) && speedSlider && speedValue && !isDraggingSpeed) {
            speedSlider.value = speed;
            speedValue.innerText = speed;
        }
    }

    if (topic === ROBOT_STATE_TOPIC) {
        let data = {};
        try {
            data = JSON.parse(msg.toString());
        } catch (e) {
            console.log("ROBOT_STATE_TOPIC JSON parse error", e);
            return;
        }

        if (data.mode !== undefined) {
            mode = data.mode;
            updateModeUI();
        }

        // Chỉ update slider khi người dùng không đang kéo
        if (data.requestedSpeed !== undefined) {
            if (speedSlider && speedValue && !isDraggingSpeed) {
                speedSlider.value = data.requestedSpeed;
                speedValue.innerText = data.requestedSpeed;
            }
        }

        if (data.appliedSpeed !== undefined && actualSpeedValue) {
            actualSpeedValue.innerText = data.appliedSpeed;
        }

        if (data.distance !== undefined && distanceValue) {
            distanceValue.innerText = `${Number(data.distance).toFixed(1)} cm`;
        }

        if (data.move !== undefined && moveValue) {
            moveValue.innerText = data.move.toUpperCase();
        }

        if (data.move !== undefined && joyStatus) {
            joyStatus.innerText = (data.move === "stop")
                ? "STOP"
                : String(data.move).toUpperCase();
        }
    }
});

/*  UI SYNC  */

function broadcastUI() {
    if (!client.connected) return;

    const data = {
        mode: mode,
        flash: flashlight,
        joy: joyStatus ? joyStatus.innerText : "STOP"
    };

    client.publish(UI_TOPIC, JSON.stringify(data));
}

/*  MODE  */

function toggleMode() {
    mode = (mode === "Auto") ? "Manual" : "Auto";

    updateModeUI();

    console.log("Toggle mode ->", mode, "| connected =", client.connected);

    if (client.connected) {
        client.publish(MODE_TOPIC, mode);
        console.log("Published mode:", mode);
    } else {
        console.log("MQTT not connected, mode not published");
    }

    broadcastUI();
}

function updateModeUI() {
    if (modeLabel) {
        modeLabel.innerText = "Mode: " + mode;
        modeLabel.classList.remove("mode-auto", "mode-manual");
        modeLabel.classList.add(mode === "Auto" ? "mode-auto" : "mode-manual");
    }

    document.querySelectorAll(".ctrl-btn")
        .forEach(b => b.disabled = (mode === "Auto"));

    if (speedSlider) {
        speedSlider.disabled = (mode === "Auto");
    }
}

/*  FLASH  */

function toggleFlash() {
    flashlight = !flashlight;

    const val = flashlight ? 255 : 0;

    if (flashBtn) {
        flashBtn.innerText = flashlight ? "Flash ON" : "Flash OFF";
        flashBtn.classList.toggle("flash-on", flashlight);
    }

    fetch(`${ESP32_URL}/control?var=led_intensity&val=${val}`)
        .catch(err => console.log("Flash request error:", err));

    broadcastUI();
}

function updateFlashUI() {
    if (!flashBtn) return;

    flashBtn.innerText = flashlight ? "Flash ON" : "Flash OFF";
    flashBtn.classList.toggle("flash-on", flashlight);
}

/*  SEND MOVE  */

function sendMove(cmd) {
    if (mode === "Auto") {
        if (joyStatus) joyStatus.innerText = "STOP";
        console.log("Still in Auto on web, not sending move");
        return;
    }

    if (joyStatus) {
        joyStatus.innerText = cmd.toUpperCase();
    }

    console.log("Send move ->", cmd, "| connected =", client.connected);

    if (client.connected) {
        client.publish(CMD_TOPIC, cmd);
        console.log("Published move:", cmd);
    } else {
        console.log("MQTT not connected, move not published");
    }

    broadcastUI();
}

/*  CONTROL BUTTON  */

function controlButtonSetup() {
    let isHolding = false;
    let holdCmd = "";

    const buttons = document.querySelectorAll(".ctrl-btn:not(.stop-btn)");
    const stopBtn = document.querySelector(".stop-btn");

    buttons.forEach(btn => {
        btn.addEventListener("pointerdown", (e) => {
            e.preventDefault();
            holdCmd = btn.dataset.cmd;
            isHolding = true;
            sendMove(holdCmd);
        });

        btn.addEventListener("pointerup", stopHold);
        btn.addEventListener("pointerleave", stopHold);
        btn.addEventListener("pointercancel", stopHold);

        btn.addEventListener("touchstart", (e) => {
            e.preventDefault();
            holdCmd = btn.dataset.cmd;
            isHolding = true;
            sendMove(holdCmd);
        }, { passive: false });

        btn.addEventListener("touchend", stopHold, { passive: false });
        btn.addEventListener("touchcancel", stopHold, { passive: false });
    });

    if (stopBtn) {
        stopBtn.addEventListener("pointerdown", (e) => {
            e.preventDefault();
            isHolding = false;
            holdCmd = "";
            sendMove("stop");
        });

        stopBtn.addEventListener("touchstart", (e) => {
            e.preventDefault();
            isHolding = false;
            holdCmd = "";
            sendMove("stop");
        }, { passive: false });
    }

    function stopHold(e) {
        if (e) e.preventDefault();

        if (isHolding) {
            isHolding = false;
            holdCmd = "";
            sendMove("stop");
        }
    }
}

controlButtonSetup();

/*  INIT DISABLE  */

document.querySelectorAll(".ctrl-btn")
    .forEach(btn => btn.disabled = true);

/*  SPEED CONTROL  */

function publishSpeedNow(val) {
    if (mode === "Manual" && client.connected) {
        client.publish(SPEED_TOPIC, String(val));
    }
}

function scheduleSpeedPublish(val) {
    pendingSpeedValue = val;

    if (speedPublishTimer) return;

    speedPublishTimer = setTimeout(() => {
        if (pendingSpeedValue !== null) {
            publishSpeedNow(pendingSpeedValue);
        }
        speedPublishTimer = null;
    }, 150); // Android thường mượt hơn với 120-180ms
}

if (speedSlider) {
    speedSlider.disabled = true;

    const onSliderInput = () => {
        let val = parseInt(speedSlider.value, 10);
        if (isNaN(val)) val = 0;

        // cập nhật local ngay để nhìn mượt
        if (speedValue) {
            speedValue.innerText = val;
        }

        // publish có tiết chế
        scheduleSpeedPublish(val);
    };

    const dragStart = () => {
        isDraggingSpeed = true;
    };

    const dragEnd = () => {
        let val = parseInt(speedSlider.value, 10);
        if (isNaN(val)) val = 0;

        isDraggingSpeed = false;

        if (speedValue) {
            speedValue.innerText = val;
        }

        // chốt giá trị cuối cùng
        publishSpeedNow(val);
    };

    speedSlider.addEventListener("input", onSliderInput);
    speedSlider.addEventListener("change", dragEnd);

    speedSlider.addEventListener("pointerdown", dragStart);
    speedSlider.addEventListener("pointerup", dragEnd);
    speedSlider.addEventListener("pointercancel", dragEnd);

    speedSlider.addEventListener("touchstart", dragStart, { passive: true });
    speedSlider.addEventListener("touchend", dragEnd, { passive: true });
    speedSlider.addEventListener("touchcancel", dragEnd, { passive: true });
}

/*  BLOCK CONTEXT MENU  */

document.addEventListener("contextmenu", (e) => {
    e.preventDefault();
});

/*  INITIAL UI  */

updateModeUI();
updateFlashUI();