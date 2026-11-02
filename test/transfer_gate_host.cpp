#include "TransferGate.h"
#include <assert.h>
#include <initializer_list>
int main() {
    TransferGate gate;
    assert(!gate.claim(TransferOwner::None));
    for (auto owner:{TransferOwner::Content,TransferOwner::BleOta,TransferOwner::WebOta}) {
        assert(gate.claim(owner));
        for (auto other:{TransferOwner::Content,TransferOwner::BleOta,TransferOwner::WebOta}) {
            assert(!gate.claim(other));
            if (other!=owner) { gate.release(other); assert(gate.owner()==owner); }
        }
        gate.release(owner); assert(gate.owner()==TransferOwner::None);
    }
}
