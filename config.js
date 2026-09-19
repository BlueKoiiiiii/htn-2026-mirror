/* Magic Mirror Config Sample
 *
 * By Michael Teeuw https://michaelteeuw.nl
 * MIT Licensed.
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
        {
    	module: "MMM-LaptopLink",
    	position: "middle_center",
    	config: {
        	messageDuration: 8000,
        	showStatus: true,
        	showSendButton: false,
        	pingText: "Ping from mirror"
    	}
	},
        // 1. MMM-TOUCHBUTTON
        {
            module: "MMM-TouchButton",
            position: "bottom_left",
            config: {
                buttons: [
{
    			name: "ping",
    			icon: "fa fa-bell",
    			notification: "WATCH_PING"
	},
                    {
                        name: "right",
                        icon: "fa fa-arrow-right",
                        notification: "PAGE_INCREMENT"
                    },
                    {
                        name: "power",
                        icon: "fa fa-power-off",
                        command: "/home/daniel/toggle_screen.sh"
                    },
                    {
                        name: "led",
                        icon: "fa fa-lightbulb-o",
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
                    // PAGE 0: Clock AND Calendar together
                     
                    // PAGE 1: Compliments
                    ["compliments"],
["clock"],
["calendar"]
                ],
                fixed: [
                    "MMM-TouchButton",
                    "MMM-LaptopLink"
                ],
            }
        },
    
        // 3. CLOCK (Moved to top_right)
        {
            module: "clock",
            position: "top_center", // Changed from top_center to top_right
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
                updateInterval: 30000, 
                fadeSpeed: 4000, 
                compliments: {
                    anytime:  ["Hey cutiful", "You look beaudorable"],
                    morning:  ["Have a good dayy", "looking cutiful today"],
                    afternoon:["Haaaaaaaaaaaaa", "You look so beaudorable"],
                    evening:  ["goodnighttttt"]
                }
            }
        },

        // 5. CALENDAR (Page 0)
        {
            module: "calendar",
            header: "My Schedule",
            position: "top_center", // Stays on the left
            config: {
		wrapEvents: true,
		fetchInterval: 60000,
                calendars: [
                    {
                        symbol: "calendar-check",
                        url: "https://calendar.google.com/calendar/ical/magicmirrorcalendar10%40gmail.com/private-0022e3933ecd79a60ffbeb19ad25a90f/basic.ics" 
                    }
                ]
            }
        },
    ]
};

/*************** DO NOT EDIT THE LINE BELOW ***************/
if (typeof module !== "undefined") { module.exports = config; }
