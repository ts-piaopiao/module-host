#include "script_fsm.h"

void TickFsm(StateContext& ctx) {
    switch (ctx.rt.state) {
        case StateId::IDLE:        StateIdle(ctx);        break;
        case StateId::CHASE:       StateChase(ctx);       break;
        case StateId::ATTACK:      StateAttack(ctx);      break;
        case StateId::ATTACK_TURN: StateAttackTurn(ctx);  break;
        case StateId::RECOVERY:    StateRecovery(ctx);    break;
    }
}
