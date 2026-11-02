#pragma once
#include <atomic>
enum class TransferOwner { None, Content, BleOta, WebOta };
class TransferGate {
public:
    bool claim(TransferOwner who) {
        if (who==TransferOwner::None) return false;
        auto expected=TransferOwner::None;
        return owner_.compare_exchange_strong(expected,who);
    }
    void release(TransferOwner who) { owner_.compare_exchange_strong(who,TransferOwner::None); }
    TransferOwner owner() const { return owner_.load(); }
private:
    std::atomic<TransferOwner> owner_{TransferOwner::None};
};
extern TransferGate transferGate;
inline bool isAnyOtaInProgress() {
    const auto owner=transferGate.owner();
    return owner==TransferOwner::BleOta || owner==TransferOwner::WebOta;
}
