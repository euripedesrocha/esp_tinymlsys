# GPTimer ping-pong between two tasks

One sender chain alternates between two independent FreeRTOS task contexts:

`GPTimer ISR → ping task → GPTimer ISR → pong task → repeat`

