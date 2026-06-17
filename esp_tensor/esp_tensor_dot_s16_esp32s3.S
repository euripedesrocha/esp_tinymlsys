#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3

    .text
    .align 4
    .global esp_tensor_dot_s16_raw_esp32s3
    .type esp_tensor_dot_s16_raw_esp32s3,@function
esp_tensor_dot_s16_raw_esp32s3:
    entry   a1,16

    movi.n  a5,0
    wur.accx_0 a5
    wur.accx_1 a5

    ee.vld.128.ip q0,a2,16
    ee.vld.128.ip q1,a3,16

    srli    a4,a4,3
    addi.n  a4,a4,-1
    loopnez a4,.Ldot_s16_loop_end
    ee.vmulas.s16.accx.ld.ip q1,a3,16,q0,q1
    ee.vld.128.ip q0,a2,16
.Ldot_s16_loop_end:

    ee.vmulas.s16.accx q0,q1
    rur.accx_0 a2
    rur.accx_1 a3
    slli    a3,a3,24
    srai    a3,a3,24
    retw.n

    .align 4
    .global esp_tensor_dot_s16_q15_esp32s3
    .type esp_tensor_dot_s16_q15_esp32s3,@function
esp_tensor_dot_s16_q15_esp32s3:
    entry   a1,16

    movi.n  a5,0
    wur.accx_0 a5
    wur.accx_1 a5

    ee.vld.128.ip q0,a2,16
    ee.vld.128.ip q1,a3,16

    srli    a4,a4,3
    addi.n  a4,a4,-1
    loopnez a4,.Ldot_s16_q15_loop_end
    ee.vmulas.s16.accx.ld.ip q1,a3,16,q0,q1
    ee.vld.128.ip q0,a2,16
.Ldot_s16_q15_loop_end:

    ee.vmulas.s16.accx q0,q1
    movi.n  a3,15
    ee.srs.accx a2,a3,0
    retw.n

#endif
