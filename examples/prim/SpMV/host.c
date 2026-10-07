#include "../../support/host.h"

#define DPU_BINARY_PATH "build/examples/SpMV"

/* Known-input application example, with explicit guest byte offsets. */
int main(void) {
    pimwasm_t *module = NULL;
    int status = 1;
    APP_CHECK(pimwasm_open(DPU_BINARY_PATH, &module, NULL) == PIMWASM_OK);
    const uint64_t input0[] = {UINT64_C(0x0), UINT64_C(0x2), UINT64_C(0x2), UINT64_C(0x4)};
    APP_CHECK(app_upload(module, 1023, input0, 4, 4) == PIMWASM_OK);
    const uint64_t input1[] = {UINT64_C(0x0), UINT64_C(0x3), UINT64_C(0x1), UINT64_C(0x2)};
    APP_CHECK(app_upload(module, 3011, input1, 4, 4) == PIMWASM_OK);
    const uint64_t input2[] = {UINT64_C(0x3fc00000), UINT64_C(0x40000000), UINT64_C(0xbf800000), UINT64_C(0x3f000000)};
    APP_CHECK(app_upload(module, 4013, input2, 4, 4) == PIMWASM_OK);
    const uint64_t input3[] = {UINT64_C(0x40000000), UINT64_C(0x40800000), UINT64_C(0x41000000), UINT64_C(0x41800000)};
    APP_CHECK(app_upload(module, 5017, input3, 4, 4) == PIMWASM_OK);
    const struct pimwasm_value args[] = {
        {.type = PIMWASM_I32, .bits = 1023},
        {.type = PIMWASM_I32, .bits = 3011},
        {.type = PIMWASM_I32, .bits = 4013},
        {.type = PIMWASM_I32, .bits = 5017},
        {.type = PIMWASM_I32, .bits = 8201},
        {.type = PIMWASM_I32, .bits = 3},
        {.type = PIMWASM_I32, .bits = 4},
        {.type = PIMWASM_I32, .bits = 4}
    };
    struct pimwasm_call call = {8, args};
    struct pimwasm_results results;
    enum pimwasm_status invoke_status = pimwasm_invoke(module, &call, &results);
    if (invoke_status != PIMWASM_OK) {
        app_fail("SpMV: %s, trap=%u",
                pimwasm_status_string(invoke_status), (unsigned)results.trap);
        goto done;
    }
    APP_CHECK(results.count == 1 && results.values[0].type == PIMWASM_I32 &&
              results.values[0].bits == 3);
    const uint64_t expected[] = {UINT64_C(0x420c0000), UINT64_C(0x0), UINT64_C(0x0)};
    APP_CHECK(app_check_output(module, 8201, expected, 3, 4));
    app_ok("SpMV");
    status = 0;
done:
    if (module) pimwasm_close(module);
    return status;
}
