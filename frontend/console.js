const maxDimension = 1000;
const frameSize = 16 * 1024;
const maxInputQueue = 1024 * 1024;
const idleMessage = "Shell runs as the Wave service account.";
const inputQueueMessage = "Terminal input queue is full. Try again in a moment.";

function loadStylesheet() {
    return new Promise((resolve, reject) => {
        const stylesheet = document.createElement("link");
        stylesheet.rel = "stylesheet";
        stylesheet.href = new URL("./terminal/xterm.css", import.meta.url);
        stylesheet.addEventListener("load", resolve, { once: true });
        stylesheet.addEventListener("error", reject, { once: true });
        document.head.append(stylesheet);
    });
}

export function createConsole(panel, copyText) {
    const mount = panel.querySelector("#wave-console-terminal");
    const status = panel.querySelector("#wave-console-status");
    const toggle = panel.querySelector("#wave-console-toggle");
    const ctrl = panel.querySelector("#wave-console-ctrl");
    const esc = panel.querySelector("#wave-console-esc");
    const encoder = new TextEncoder();
    let assets;
    let session;
    let terminal;
    let fitAddon;
    let controlPending = false;

    function setControlPending(pending) {
        controlPending = pending;
        ctrl.setAttribute("aria-pressed", String(pending));
    }

    function updateControls(message) {
        toggle.textContent = session ? "Close terminal" : "Open terminal";
        ctrl.hidden = esc.hidden = !session?.ready;
        status.textContent = message;
        if (!session?.ready) setControlPending(false);
    }

    function finish(connection, message) {
        if (session !== connection) return;
        session = undefined;
        connection?.socket?.close();
        if (terminal) terminal.options.disableStdin = true;
        updateControls(message);
    }

    function discardTerminal() {
        terminal?.dispose();
        terminal = undefined;
        fitAddon = undefined;
    }

    function disconnect() {
        finish(session, idleMessage);
        discardTerminal();
    }

    function sendInput(bytes) {
        const socket = session?.socket;
        if (!session?.ready || socket?.readyState !== WebSocket.OPEN) return;
        if (socket.bufferedAmount + bytes.length > maxInputQueue) {
            status.textContent = inputQueueMessage;
            return;
        }
        for (let offset = 0; offset < bytes.length; offset += frameSize) {
            socket.send(bytes.subarray(offset, offset + frameSize));
        }
        if (status.textContent === inputQueueMessage) status.textContent = "Connected";
    }

    function sendText(data) {
        if (controlPending) {
            setControlPending(false);
            if (/^[@A-Z\[\\\]^_a-z]$/.test(data)) {
                data = String.fromCharCode(data.toUpperCase().charCodeAt(0) & 31);
            } else if (data === "?") {
                data = "\x7f";
            } else if (data === " ") {
                data = "\x00";
            }
        }
        sendInput(encoder.encode(data));
    }

    function sendSize(connection) {
        if (connection.socket.readyState !== WebSocket.OPEN) return;
        const { cols, rows } = terminal;
        if (connection.cols === cols && connection.rows === rows) return;
        connection.socket.send(JSON.stringify({ type: "resize", cols, rows }));
        connection.cols = cols;
        connection.rows = rows;
    }

    function receive(connection, data) {
        if (session !== connection) return;
        if (data instanceof ArrayBuffer) {
            try {
                terminal.write(new Uint8Array(data));
            } catch {
                finish(connection, "Terminal output is too fast to display.");
            }
            return;
        }

        let reply;
        try {
            reply = JSON.parse(data);
        } catch {
            finish(connection, "Unexpected terminal response.");
            return;
        }
        if (reply?.type === "ready") {
            connection.ready = true;
            terminal.options.disableStdin = false;
            updateControls("Connected");
        } else if (reply?.type === "exit" || reply?.type === "error") {
            finish(connection, typeof reply.message === "string" ? reply.message : "Terminal ended.");
        }
    }

    function connect(connection) {
        const url = new URL("/ws/console", window.location.href);
        url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
        try {
            connection.socket = new WebSocket(url);
        } catch {
            finish(connection, "Could not connect to the terminal.");
            return;
        }
        const socket = connection.socket;
        socket.binaryType = "arraybuffer";
        socket.addEventListener("open", () => {
            if (session === connection) sendSize(connection);
        });
        socket.addEventListener("message", event => receive(connection, event.data));
        socket.addEventListener("error", () => finish(connection, "Could not connect to the terminal."));
        socket.addEventListener("close", event => {
            finish(connection, event.reason || "Terminal disconnected.");
        });
    }

    function fit() {
        if (!terminal || panel.hidden || !mount.clientWidth || !mount.clientHeight) return;
        const dimensions = fitAddon.proposeDimensions();
        if (!dimensions) return;
        terminal.resize(Math.min(dimensions.cols, maxDimension), Math.min(dimensions.rows, maxDimension));
        if (!session) return;
        if (session.socket) sendSize(session);
        else connect(session);
    }

    async function open() {
        discardTerminal();
        const connection = {};
        session = connection;
        updateControls("Opening terminal…");
        try {
            assets ??= Promise.all([
                import("./terminal/xterm.js"), import("./terminal/addon-fit.js"), loadStylesheet()
            ]);
            const [{ Terminal }, { FitAddon }] = await assets;
            if (session !== connection) return;
            terminal = new Terminal({
                cursorBlink: true,
                disableStdin: true,
                fontFamily: 'ui-monospace, "Cascadia Mono", "Liberation Mono", monospace',
                fontSize: 14,
                minimumContrastRatio: panel.hasAttribute("data-high-contrast") ? 7 : 4.5,
                scrollback: 5000,
                theme: { background: "#050506", foreground: "#eaebff", cursor: "#f3814a" }
            });
            fitAddon = new FitAddon();
            terminal.loadAddon(fitAddon);
            terminal.open(mount);
            terminal.onData(sendText);
            terminal.onBinary(data => sendInput(Uint8Array.from(data, char => char.charCodeAt(0))));
            terminal.attachCustomKeyEventHandler(event => {
                if (event.type === "keydown" && event.ctrlKey && event.shiftKey && event.code === "KeyC" &&
                    terminal.hasSelection()) {
                    event.preventDefault();
                    copyText(terminal.getSelection());
                    return false;
                }
                return session?.ready || event.key !== "Tab";
            });
            if (!panel.hidden) terminal.focus();
            fit();
        } catch {
            if (session === connection) {
                finish(connection, "Terminal could not load. Reload Wave to retry.");
                discardTerminal();
            }
        }
    }

    ctrl.addEventListener("click", () => {
        setControlPending(!controlPending);
        terminal?.focus();
    });
    esc.addEventListener("click", () => {
        setControlPending(false);
        sendText("\x1b");
        terminal?.focus();
    });
    toggle.addEventListener("click", () => {
        if (session) disconnect();
        else open();
    });
    toggle.disabled = false;
    new ResizeObserver(fit).observe(mount);
    window.addEventListener("pagehide", disconnect);

    return {
        disconnect,
        setHighContrast(highContrast) {
            const ratio = highContrast ? 7 : 4.5;
            if (terminal && terminal.options.minimumContrastRatio !== ratio) {
                terminal.options.minimumContrastRatio = ratio;
            }
        }
    };
}
