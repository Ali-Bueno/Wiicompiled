#ifndef MKW_ACCESSIBILITY_UPDATE_CHECK_H
#define MKW_ACCESSIBILITY_UPDATE_CHECK_H

namespace a11y::update {

// Launches the background check, if [accessibility] check_updates is on and the installer wrote
// the [update] keys. Never blocks and never fails loudly: with no network, nothing is announced.
void Start();

// Guest thread. Announces the result once, asks about each available update in turn, and acts on
// the answers.
void Tick();

// Host event thread. Answers the pending question; false when there is none, so the caller can
// pass the key on. Only the first answer to a question counts.
bool OnAnswer(bool accept);

}  // namespace a11y::update

#endif  // MKW_ACCESSIBILITY_UPDATE_CHECK_H
