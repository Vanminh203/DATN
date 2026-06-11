/*  CONFIG  */
const ESP32_URL = "http://10.102.183.100";
const PI_STREAM = "http://10.102.183.12:5000/video";
const MQTT_BROKER = "ws://10.102.183.12:9001";

let mode = "Auto";
let flashlight = false;
const AUTO_SET_SPEED = 5;
const DEFAULT_MANUAL_SPEED = 30;

let manualSpeedValue = DEFAULT_MANUAL_SPEED;
/*  MQTT TOPICS  */
const CMD_TOPIC         = "control/move";
const MODE_TOPIC        = "control/mode";
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

function setSpeedUI(value) {
    if (!speedSlider || !speedValue) return;

    speedSlider.value = value;
    speedValue.innerText = value;
}

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

    if (topic === SPEED_TOPIC) {
            const speed = parseInt(msg.toString(), 10);

    if (isNaN(speed)) return;

    if (mode === "Manual") {
        manualSpeedValue = speed;

        if (!isDraggingSpeed) {
            setSpeedUI(manualSpeedValue);
        }
        } else {
            setSpeedUI(AUTO_SET_SPEED);
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

        if (data.mode !== undefined && data.mode !== mode) {
            mode = data.mode;
            updateModeUI();
        }
        if (data.requestedSpeed !== undefined) {
            if (mode === "Manual") {
                manualSpeedValue = Number(data.requestedSpeed);
        if (!isDraggingSpeed) {
            setSpeedUI(manualSpeedValue);
        }
        } else {
            setSpeedUI(AUTO_SET_SPEED);
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

/*  MODE  */
function toggleMode() {
    mode = (mode === "Auto") ? "Manual" : "Auto";

    updateModeUI();

    console.log("Toggle mode ->", mode, "| connected =", client.connected);

    if (client.connected) {
        client.publish(MODE_TOPIC, mode);
        console.log("Published mode:", mode);
        client.publish(CMD_TOPIC, "stop");
    } else {
        console.log("MQTT not connected, mode not published");
    }

}
function updateModeUI() {
    const isAuto = mode === "Auto";

    if (modeLabel) {
        modeLabel.innerText = "Mode: " + mode;
        modeLabel.classList.remove("mode-auto", "mode-manual");
        modeLabel.classList.add(isAuto ? "mode-auto" : "mode-manual");
    }

    document.querySelectorAll(".ctrl-btn")
        .forEach(btn => btn.disabled = isAuto);

    if (speedSlider) {
        speedSlider.disabled = isAuto;
    }

    if (isAuto) {
        setSpeedUI(AUTO_SET_SPEED);
    } else if (!isDraggingSpeed) {
        setSpeedUI(manualSpeedValue);
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
    }, 150); 
}

if (speedSlider) {
    speedSlider.disabled = true;

    const onSliderInput = () => {
    if (mode !== "Manual") return;

    let val = parseInt(speedSlider.value, 10);
    if (isNaN(val)) val = 0;

    manualSpeedValue = val;
    setSpeedUI(manualSpeedValue);

    scheduleSpeedPublish(manualSpeedValue);
};

    const dragStart = () => {
        isDraggingSpeed = true;
    };

    const dragEnd = () => {
        if (mode !== "Manual") return;

        let val = parseInt(speedSlider.value, 10);
        if (isNaN(val)) val = 0;

        isDraggingSpeed = false;
        manualSpeedValue = val;

        setSpeedUI(manualSpeedValue);
        publishSpeedNow(manualSpeedValue);
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
