/* Browser/Electron side: render text, never execute received code or HTML. */
Module.register("MMM-LaptopLink", {
  defaults: {
    messageDuration: 8000,
    showStatus: true,
    showSendButton: true,
    pingText: "Ping from mirror"
  },

  start() {
    this.message = "";
    this.statusText = "Waiting for laptop link...";
    this.clearTimer = null;
    this.sendStatus = "";
    this.seen = new Set();
  },

  notificationReceived(notification) {
    if (notification === "DOM_OBJECTS_CREATED") {
      // Establish the helper socket only after the UI is ready to accept messages.
      this.sendSocketNotification("LAPTOPLINK_READY", {});
    } else if (notification === "WATCH_PING") {
      this.sendPing();
    }
  },

  sendPing() {
    this.sendSocketNotification("LAPTOPLINK_SEND", {text: this.config.pingText || "Ping from mirror"});
  },

  socketNotificationReceived(notification, payload) {
    if (notification === "LAPTOPLINK_STATUS" && payload) {
      this.statusText = payload.text;
      this.updateDom(0);
    } else if (notification === "LAPTOPLINK_SEND_STATUS" && payload) {
      this.sendStatus = payload.text;
      this.updateDom(0);
    } else if (notification === "LAPTOPLINK_MESSAGE" && payload &&
               typeof payload.id === "string" && typeof payload.text === "string") {
      if (!this.seen.has(payload.id)) {
        this.seen.add(payload.id);
        if (this.seen.size > 100) this.seen.delete(this.seen.values().next().value);
        this.message = payload.text;
        this.updateDom(0);
        clearTimeout(this.clearTimer);
        const configuredDuration = Number(this.config.messageDuration);
        const duration = Number.isFinite(configuredDuration) && configuredDuration >= 1000
          ? configuredDuration : 8000;
        this.clearTimer = setTimeout(() => {
          this.message = "";
          this.updateDom(0);
        }, duration);
      }
      // Means the UI accepted the message and requested rendering. It does not
      // certify physical screen power, completed rendering, or a human reading it.
      this.sendSocketNotification("LAPTOPLINK_UI_RECEIVED", { id: payload.id });
    }
  },

  getStyles() {
    return [this.file("MMM-LaptopLink.css")];
  },

  getDom() {
    const wrapper = document.createElement("div");
    wrapper.className = "laptoplink";
    if (this.message) {
      const banner = document.createElement("div");
      banner.className = "laptoplink-message";
      banner.textContent = this.message;
      wrapper.appendChild(banner);
    }
    if (this.config.showSendButton) {
      const button = document.createElement("button");
      button.className = "laptoplink-button";
      button.textContent = "Notify watch";
      button.addEventListener("click", () => this.sendPing());
      wrapper.appendChild(button);
    }
    if (this.sendStatus) {
      const delivery = document.createElement("div");
      delivery.className = "laptoplink-delivery";
      delivery.textContent = this.sendStatus;
      wrapper.appendChild(delivery);
    }
    if (this.config.showStatus) {
      const status = document.createElement("div");
      status.className = "laptoplink-status";
      status.textContent = this.statusText;
      wrapper.appendChild(status);
    }
    return wrapper;
  }
});
