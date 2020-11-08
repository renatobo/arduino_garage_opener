// Compile date for info page
const char compile_date[] PROGMEM = __DATE__ " " __TIME__ " EST";
const char bonogarversion[] PROGMEM = "Version 5.3.0";

// Page content
const char bonogarhead[] PROGMEM ="<html><head><meta name=viewport content='width=device-width, initial-scale=1.0' /></head><body>";
const char bonogarfstart[] PROGMEM ="<br><a href=/>Home</a><footer><i>Date: ";
const char bonogarfend[] PROGMEM = "</i><br>&#128584; &#128585; &#128586;</footer></body><html>";
const char bonogarTZ[] PROGMEM = " EST<br>";
const char bonopushtopen[] PROGMEM = "<form action=switch method=POST><button type=submit style='width: 200;height:50;'>Open/Close garage door</button></form>";
