/* Bidirectional mirror client. LAPTOP is the only relay. No inbound Pi listener. */
const NodeHelper = require("node_helper");
const net = require("net");
const fs = require("fs");
const path = require("path");
const os = require("os");
const crypto = require("crypto");

const MAX_FRAME = 4096;
const MAX_TEXT = 500;

module.exports = NodeHelper.create({
  start() {
    this.stopping = false;
    this.socket = null;
    this.retryTimer = null;
    this.retryDelay = 2000;
    this.connected = false;
    this.config = null;
    this.statusText = "Waiting for mirror UI";
    this.awaitingUI = new Map();
    this.seen = new Set();
    this.outbound = new Map();
  },

  socketNotificationReceived(notification, payload) {
    if (notification === "LAPTOPLINK_READY") {
      this.setStatus(this.connected, this.statusText);
      if (!this.config) {
        try {
          // Keep the shared secret OUTSIDE MagicMirror's web-served folders.
          const filename = path.join(os.homedir(), ".config", "mirror-link", "connection.json");
          const config = JSON.parse(fs.readFileSync(filename, "utf8").replace(/^\uFEFF/, ""));
          if (net.isIP(config.host) !== 4 || !Number.isInteger(config.port) ||
              config.port < 1024 || config.port > 65535 || config.device_id !== "mirror-01" ||
              typeof config.token !== "string" || !/^[0-9a-f]{64}$/.test(config.token)) {
            throw new Error("Invalid connection settings");
          }
          this.config = config;
        } catch (error) {
          this.setStatus(false, "Setup error: check ~/.config/mirror-link/connection.json");
          console.error("[MMM-LaptopLink] Cannot load connection settings:", error.code || error.name);
          return;
        }
      }
      if (!this.socket && !this.retryTimer) this.connectLaptop();
    } else if (notification === "LAPTOPLINK_SEND") {
      this.sendWatchPing(payload);
    } else if (notification === "LAPTOPLINK_UI_RECEIVED" && payload && typeof payload.id === "string") {
      const receivedAt = this.awaitingUI.get(payload.id);
      if (receivedAt === undefined) return;
      this.awaitingUI.delete(payload.id);
      this.seen.add(payload.id);
      if (this.seen.size > 100) this.seen.delete(this.seen.values().next().value);
      this.sendAck(payload.id);
    }
  },

  deliveryStatus(text) {
    this.sendSocketNotification("LAPTOPLINK_SEND_STATUS", { text });
  },

  sendWatchPing(payload) {
    if (!this.connected) {
      this.deliveryStatus("Not sent: laptop offline");
      return;
    }
    if (this.outbound.size) {
      this.deliveryStatus("Still waiting for the previous message");
      return;
    }
    const text = payload && typeof payload.text === "string" ? payload.text.trim() : "Ping from mirror";
    if (!text || text.includes("\0") || Array.from(text).length > MAX_TEXT) return;
    const id = `mirror-${crypto.randomBytes(12).toString("hex")}`;
    const timer = setTimeout(() => {
      this.outbound.delete(id);
      this.deliveryStatus("Not confirmed: no watch result");
    }, 12000);
    this.outbound.set(id, timer);
    this.deliveryStatus("Sending to watch...");
    if (!this.send({v: 1, type: "send", id, to: "watch-01", text})) {
      clearTimeout(timer);
      this.outbound.delete(id);
      this.deliveryStatus("Not confirmed: connection lost");
    }
  },

  failOutbound() {
    const hadPending = this.outbound.size > 0;
    for (const timer of this.outbound.values()) clearTimeout(timer);
    this.outbound.clear();
    if (hadPending) this.deliveryStatus("Not confirmed: laptop disconnected");
  },

  setStatus(connected, text) {
    this.connected = connected;
    this.statusText = text;
    this.sendSocketNotification("LAPTOPLINK_STATUS", { connected, text });
  },

  send(message) {
    const socket = this.socket;
    if (!socket || socket.destroyed || !socket.writable) return false;
    const data = Buffer.from(JSON.stringify(message) + "\n", "utf8");
    if (data.length > MAX_FRAME || socket.writableLength > MAX_FRAME * 4) {
      socket.destroy();
      return false;
    }
    socket.write(data);
    return true;
  },

  sendAck(id) {
    if (this.connected) {
      this.send({ v: 1, type: "ack", ack_for: id, status: "ui_received" });
    }
  },

  connectLaptop() {
    if (this.stopping || !this.config) return;
    this.setStatus(false, "Connecting to laptop...");
    const socket = net.createConnection({ host: this.config.host, port: this.config.port });
    this.socket = socket;
    socket.setNoDelay(true);
    socket.setKeepAlive(true, 10000);
    let buffer = Buffer.alloc(0);
    let authenticated = false;
    let lastReceived = Date.now();
    let failure = "Laptop disconnected";
    const deadline = setTimeout(() => {
      failure = "Laptop connection/authentication timed out";
      socket.destroy();
    }, 7000);
    // Check incoming activity explicitly: outgoing writes must not hide a dead peer.
    const heartbeat = setInterval(() => {
      if (Date.now() - lastReceived > 35000) {
        failure = "Laptop heartbeat timed out";
        socket.destroy();
        return;
      }
      if (authenticated) this.send({ v: 1, type: "ping" });
      for (const [id, receivedAt] of this.awaitingUI) {
        if (Date.now() - receivedAt > 10000) this.awaitingUI.delete(id);
      }
    }, 10000);

    socket.on("connect", () => {
      this.send({ v: 1, type: "hello", device_id: this.config.device_id, token: this.config.token });
    });

    socket.on("data", (chunk) => {
      buffer = Buffer.concat([buffer, chunk]);
      let newline;
      while ((newline = buffer.indexOf(10)) !== -1) {
        if (newline + 1 > MAX_FRAME) {
          failure = "Laptop sent an oversized frame";
          socket.destroy();
          return;
        }
        const line = buffer.subarray(0, newline);
        buffer = buffer.subarray(newline + 1);
        let message;
        try {
          message = JSON.parse(line.toString("utf8"));
          if (!message || Array.isArray(message) || message.v !== 1) throw new Error();
        } catch (_) {
          failure = "Laptop sent invalid JSON";
          socket.destroy();
          return;
        }
        lastReceived = Date.now();
        if (message.type === "error") {
          failure = "Authentication/protocol rejected; check the matching config files";
          socket.destroy();
          return;
        }
        if (!authenticated) {
          if (message.type !== "welcome" || message.device_id !== this.config.device_id) {
            failure = "Unexpected laptop handshake";
            socket.destroy();
            return;
          }
          authenticated = true;
          clearTimeout(deadline);
          this.retryDelay = 2000;
          this.setStatus(true, "Laptop connected");
          console.log("[MMM-LaptopLink] Connected to laptop");
        } else if (message.type === "pong") {
          // Valid incoming activity; no response needed.
        } else if (message.type === "result") {
          const timer = this.outbound.get(message.request_id);
          if (timer !== undefined && message.to === "watch-01") {
            clearTimeout(timer);
            this.outbound.delete(message.request_id);
            const labels = {
              delivered: "Watch received the message",
              offline: "Not sent: watch offline",
              unconfirmed: "Not confirmed by watch",
              busy: "Not sent: relay busy"
            };
            this.deliveryStatus(labels[message.status] || "Not confirmed: invalid result");
          }
        } else if (message.type === "notify") {
          if (typeof message.id !== "string" || !/^[A-Za-z0-9_-]{1,100}$/.test(message.id) ||
              !["laptop", "watch-01"].includes(message.from) || message.to !== this.config.device_id ||
              typeof message.text !== "string" || message.text.trim().length === 0 ||
              Array.from(message.text).length > MAX_TEXT || message.text.includes("\0")) {
            failure = "Laptop sent an invalid notification";
            socket.destroy();
            return;
          }
          if (this.seen.has(message.id)) {
            this.sendAck(message.id);
          } else if (!this.awaitingUI.has(message.id)) {
            if (this.awaitingUI.size >= 20) {
              failure = "Too many notifications waiting for mirror UI";
              socket.destroy();
              return;
            }
            this.awaitingUI.set(message.id, Date.now());
            this.sendSocketNotification("LAPTOPLINK_MESSAGE", { id: message.id, text: message.text });
          }
        } else {
          failure = "Laptop sent an unsupported message type";
          socket.destroy();
          return;
        }
      }
      if (buffer.length >= MAX_FRAME) {
        failure = "Laptop sent an incomplete/oversized frame";
        socket.destroy();
      }
    });

    socket.on("error", (error) => {
      failure = `Laptop unavailable (${error.code || "network error"})`;
    });

    socket.on("close", () => {
      clearTimeout(deadline);
      clearInterval(heartbeat);
      if (this.socket !== socket) return;
      this.socket = null;
      this.failOutbound();
      this.awaitingUI.clear();
      this.setStatus(false, failure);
      if (!this.stopping) {
        const delay = this.retryDelay;
        this.retryDelay = Math.min(this.retryDelay * 2, 30000);
        this.retryTimer = setTimeout(() => {
          this.retryTimer = null;
          this.connectLaptop();
        }, delay);
      }
    });
  },

  stop() {
    this.stopping = true;
    this.failOutbound();
    clearTimeout(this.retryTimer);
    if (this.socket) this.socket.destroy();
  }
});
