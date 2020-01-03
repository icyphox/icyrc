#define VERSION "icyrc 0.1 (https://github.com/icyphox/icyrc)"

#define SCROLL   15
#define INDENT   25

/* uncomment to enable date formatting; prepends to each msg */
// #define DATEFMT  "%H:%M"

/* normal msg    "nick    msg" */
#define PFMT     "%-15s   %s"
/* action msg    "nick    msg" */
#define AFMT     "* %-15s %s"
/* highlight msg  "nick   msg" */
#define PFMTHIGH "%-15s]  %s"

/* command that STDOUTs a password in a single line */
#define PWCMD    "pw -s ircpass"

/* server */
#define SRV      "irc.icyphox.sh"
/* port */
#define PORT     "6666"

