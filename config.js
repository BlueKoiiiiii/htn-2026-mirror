/* Magic Mirror Config Sample
 *
 * By Michael Teeuw https://michaelteeuw.nl
 * MIT Licensed.
 *
 * For more information on how to configure the MagicMirror,
 * visit https://github.com/MichMich/MagicMirror/tree/master/config
 *
 */

let config = {
    address: "localhost", 
    port: 8080,
    basePath: "/", 
    ipWhitelist: ["127.0.0.1", "::ffff:127.0.0.1", "::1"], 

    useHttps: false, 
    httpsPrivateKey: "", 
    httpsCertificate: "", 

    language: "en",
    locale: "en-US",
    logLevel: ["INFO", "LOG", "WARN", "ERROR"],
    timeFormat: 24,
    units: "metric",

    modules: [
        // 1. MMM-TOUCHBUTTON (Invisible Touch Zones)
        {
            module: "MMM-TouchButton",
            position: "bottom_left",
            config: {
                buttons: [
                    {
                        name: "right",
                        icon: "fa fa-arrow-right", // Icon hidden by CSS
                        notification: "PAGE_INCREMENT"
                    },
                    {
                        name: "power",
                        icon: "fa fa-power-off",   // Icon hidden by CSS
                        command: "/home/daniel/toggle_screen.sh"
                    },
                    {
                        name: "led",
                        icon: "fa fa-lightbulb-o", // Icon hidden by CSS
                        command: "/home/daniel/toggle_led.sh"
                    }
                ]
            },
        },

        // 2. MMM-PAGES (Page Rotation)
        {
            module: "MMM-pages",
            config: {
                modules: [
                    ["clock"],          // Page 0
                    ["compliments"],    // Page 1
                ],
                fixed: [                // Always active/shown
                    "MMM-TouchButton"
                ],
            }
        },
    
        // 3. CLOCK (Page 0)
        {
            module: "clock",
            position: "top_center", 
            classes: "main", 
            config: {
                // Clock config options here
            }
        },

        // 4. COMPLIMENTS (Page 1)
        {
            module: "compliments",
            position: "top_center",
            config: {
                updateInterval: 30000, // 30 seconds
                fadeSpeed: 4000,       // 4 seconds
                compliments: {
                    anytime: [
                        "Hey Cutiful!",
                        "You look Beaudorable"
                    ],
                    morning: [
                        "Have a cutiful day!",
                        "Hello, Beautie!"
                    ],
                    afternoon: [
                        "Hello, beauty!",
                        "Looking good today!",
                        "You look Adorabeau"
                    ],
                    evening: [
                        "Wow, you look hot!",
                        "Have a good night"
                    ]
                }
            }
        },
    ]
};

/*************** DO NOT EDIT THE LINE BELOW ***************/
if (typeof module !== "undefined") { module.exports = config; }

