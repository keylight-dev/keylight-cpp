#pragma once
#define KEYLIGHT_SDK_VERSION "0.2.1"

// Identifies this SDK to the backend, sent as `sdk` alongside `platform`.
// Server cap is 16 characters (zod `z.string().max(16)`).
#define KEYLIGHT_SDK_ID "cpp"
