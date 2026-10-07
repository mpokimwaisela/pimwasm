#include "../../support/host.h"

#define DPU_BINARY_PATH "build/examples/HST"

/* Known-input application example, with explicit guest byte offsets. */
int main(void) {
    pimwasm_t *module = NULL;
    int status = 1;
    APP_CHECK(pimwasm_open(DPU_BINARY_PATH, &module, NULL) == PIMWASM_OK);
    const uint64_t input0[] = {UINT64_C(0x0), UINT64_C(0x3ff), UINT64_C(0x400), UINT64_C(0x800), UINT64_C(0xfff), UINT64_C(0x1000)};
    APP_CHECK(app_upload(module, 1023, input0, 6, 4) == PIMWASM_OK);
    const struct pimwasm_value args[] = {
        {.type = PIMWASM_I32, .bits = 1023},
        {.type = PIMWASM_I32, .bits = 8201},
        {.type = PIMWASM_I32, .bits = 6},
        {.type = PIMWASM_I32, .bits = 4}
    };
    struct pimwasm_call call = {4, args};
    struct pimwasm_results results;
    enum pimwasm_status invoke_status = pimwasm_invoke(module, &call, &results);
    if (invoke_status != PIMWASM_OK) {
        app_fail("HST: %s, trap=%u",
                pimwasm_status_string(invoke_status), (unsigned)results.trap);
        goto done;
    }
    APP_CHECK(results.count == 1 && results.values[0].type == PIMWASM_I32 &&
              results.values[0].bits == 6);
    const uint64_t expected[] = {UINT64_C(0x2), UINT64_C(0x1), UINT64_C(0x1), UINT64_C(0x2)};
    APP_CHECK(app_check_output(module, 8201, expected, 4, 4));
    app_ok("HST");
    status = 0;
done:
    if (module) pimwasm_close(module);
    return status;
}
