# Wyoming Test Tools

This repo contains tools I made for better understanding of Wyoming protocol, with proof of concept satellite implementation in C.
This is part of PineVox voice assistant firmware effort.

# Tools

- `wyoming_satellite_poc` - contains proof of concept satellite written in C and can be run on Desktop. It uses cJSON and AliOS ring_buffer implementation, as that's what will be used in PineVox (since it uses AliOS).
- `wyoming-test` - contains test Node.JS script, which emulates Wyoming server to some level. You can use it to connect to Python Wyoming satellite implementation.