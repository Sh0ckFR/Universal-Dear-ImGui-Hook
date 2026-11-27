/* The C interface, compiled as C: uih_host --c-api starts the hook through
 * it, and checks its defaults and names first; with --remote, the tools of
 * tests/remote_tools.h are registered through it, and checked the same way. */
#include "uih_c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* NULL when the defaults and the names are those of include/uih.h. */
const char* uih_test_check_c_api(void) {
    uih_config config;
    memset(&config, 0xAB, sizeof(config));
    uih_config_init(&config);
    if (config.backend != UIH_BACKEND_NONE || config.scale != 0.0f || config.remote_control || config.remote_memory ||
        config.ai_advanced_features || config.debug_log || config.render || config.setup)
        return "uih_config_init() does not set the defaults of uih::Config";
    if (config.menu_buttons != (UIH_GAMEPAD_L3 | UIH_GAMEPAD_R3) || !config.remote_pipe || strcmp(config.remote_pipe, "universal-imgui-hook") != 0)
        return "uih_config_init() does not set the menu buttons or the pipe of uih::Config";
#ifdef _WIN32
    if (config.menu_key != 0x2D || config.unload_key != 0x7A)
        return "uih_config_init() does not set INSERT and F11";
#endif
    if (strcmp(uih_backend_name(UIH_BACKEND_D3D11), "D3D11") != 0 || strcmp(uih_backend_name(UIH_BACKEND_VULKAN), "Vulkan") != 0)
        return "uih_backend_name() does not name the backends";
    if (uih_capture_frame(NULL, NULL, 0))
        return "uih_capture_frame() accepts no callback";
    {
        char small[4];
        char whole[16];
        size_t length = uih_tool_argument("{\"k\": \"abcdef\", \"n\": [1, 2]}", "k", small, sizeof(small));
        if (length != 6 || strcmp(small, "abc") != 0 || uih_tool_argument("{\"k\": 1}", "k", NULL, 0) != 1)
            return "uih_tool_argument() does not cut the value or give its length as snprintf() does";
        if (uih_tool_argument("{\"n\": [1, 2]}", "n", whole, sizeof(whole)) != 6 || strcmp(whole, "[1, 2]") != 0 ||
            uih_tool_argument("{}", "k", whole, sizeof(whole)) != 0 || whole[0] != '\0')
            return "uih_tool_argument() does not give the JSON of a value, or an empty one for a missing key";
    }
    return NULL;
}

/* Called by test_add, in the frame of the overlay. */
static void (*g_ran)(void);

/* test_add and test_fail, as RegisterTestTools() of tests/remote_tools.h. */
static void TestAdd(const char* arguments, uih_tool_result* result, void* context) {
    char a[64];
    char b[64];
    char label[256];
    char nested[256];
    char text[640];
    uih_tool_argument(arguments, "a", a, sizeof(a));
    uih_tool_argument(arguments, "b", b, sizeof(b));
    uih_tool_argument(arguments, "label", label, sizeof(label));
    uih_tool_argument(arguments, "nested", nested, sizeof(nested));
    if (context == &g_ran && g_ran)
        g_ran();
    snprintf(text, sizeof(text), "%s = %g\nnested: %s", label, atof(a) + atof(b), nested);
    uih_tool_answer(result, text);
}

static void TestFail(const char* arguments, uih_tool_result* result, void* context) {
    (void)arguments;
    (void)context;
    uih_tool_answer(result, "not this one");
    uih_tool_fail(result, "the test tool failed\non purpose");
}

static void TestGone(const char* arguments, uih_tool_result* result, void* context) {
    (void)arguments;
    (void)context;
    uih_tool_answer(result, "gone");
}

/* `ran` is called by test_add, in the frame of the overlay; its context
 * checks that `context` reaches the callback. */
bool uih_test_register_c_tools(void (*ran)(void)) {
    bool added;
    g_ran = ran;
    added = uih_register_tool(
        "test_add", "Adds a and b, and says the sum with a label. Quotes \" and a\nnew line must reach the assistant escaped.",
        "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"number\"},\"b\":{\"type\":\"number\"},\"label\":{\"type\":\"string\"}},\"required\":[\"a\",\"b\"]}",
        TestAdd, &g_ran);
    bool failing = uih_register_tool("test_fail", "Always fails.", NULL, TestFail, NULL);
    bool replaced = uih_register_tool("test_gone", "First.", NULL, TestGone, NULL) && uih_register_tool("test_gone", "Second.", NULL, TestGone, NULL);
    bool refused;
    uih_unregister_tool("test_gone");
    refused = !uih_register_tool("bad name", "", NULL, TestGone, NULL) && !uih_register_tool("bad_schema", "", "[1, 2]", TestGone, NULL) &&
              !uih_register_tool("no_call", "", NULL, NULL, NULL);
    return added && failing && replaced && refused;
}

/* Starts the hook as a C program does; `pipe` with the remote control on. */
void uih_test_start_c_api(void (*render)(void), void (*setup)(void), float scale, const char* pipe, bool memory, bool advanced) {
    uih_config config;
    uih_config_init(&config);
    config.render = render;
    config.setup = setup;
    config.scale = scale;
    config.debug_log = true;
    if (pipe) {
        config.remote_control = true;
        config.remote_pipe = pipe;
        config.remote_memory = memory;
        config.ai_advanced_features = advanced;
    }
    uih_start(&config);
}
