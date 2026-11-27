/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "AbortSignal.h"

#include "mozilla/dom/Event.h"
#include "mozilla/dom/AbortSignalBinding.h"
#include "nsComponentManagerUtils.h"
#include "nsITimer.h"

namespace mozilla {
namespace dom {

namespace {

class AbortTimeoutCallback final : public nsITimerCallback
{
public:
  NS_DECL_ISUPPORTS

  AbortTimeoutCallback(AbortSignal* aSignal, nsITimer* aTimer)
    : mSignal(aSignal)
    , mTimer(aTimer)
  {}

  NS_IMETHOD Notify(nsITimer* aTimer) override
  {
    mTimer = nullptr;
    RefPtr<AbortSignal> signal = mSignal.forget();
    if (signal) {
      signal->Abort();
    }
    return NS_OK;
  }

private:
  ~AbortTimeoutCallback() = default;
  RefPtr<AbortSignal> mSignal;
  nsCOMPtr<nsITimer> mTimer;
};

NS_IMPL_ISUPPORTS(AbortTimeoutCallback, nsITimerCallback)

} // anonymous namespace

NS_IMPL_CYCLE_COLLECTION_CLASS(AbortSignal)

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(AbortSignal,
                                                  DOMEventTargetHelper)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(AbortSignal,
                                                DOMEventTargetHelper)
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(AbortSignal)
NS_INTERFACE_MAP_END_INHERITING(DOMEventTargetHelper)

NS_IMPL_ADDREF_INHERITED(AbortSignal, DOMEventTargetHelper)
NS_IMPL_RELEASE_INHERITED(AbortSignal, DOMEventTargetHelper)

AbortSignal::AbortSignal(nsIGlobalObject* aGlobalObject,
                         bool aAborted)
  : DOMEventTargetHelper(aGlobalObject)
  , mAborted(aAborted)
{}

AbortSignal::AbortSignal(bool aAborted)
  : mAborted(aAborted)
{}

JSObject*
AbortSignal::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return AbortSignalBinding::Wrap(aCx, this, aGivenProto);
}

already_AddRefed<AbortSignal> AbortSignal::Abort(GlobalObject& aGlobal) {
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(aGlobal.GetAsSupports());
  RefPtr<AbortSignal> abortSignal = new AbortSignal(global, true);
  return abortSignal.forget();
}

already_AddRefed<AbortSignal> AbortSignal::Timeout(GlobalObject& aGlobal, uint64_t aMilliseconds) {
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(aGlobal.GetAsSupports());
  RefPtr<AbortSignal> abortSignal = new AbortSignal(global, false);

  nsCOMPtr<nsITimer> timer = do_CreateInstance(NS_TIMER_CONTRACTID);
  if (timer) {
    uint32_t delay = aMilliseconds > UINT32_MAX
                       ? UINT32_MAX
                       : static_cast<uint32_t>(aMilliseconds);
    RefPtr<AbortTimeoutCallback> cb = new AbortTimeoutCallback(abortSignal, timer);
    if (NS_FAILED(timer->InitWithCallback(cb, delay, nsITimer::TYPE_ONE_SHOT))) {
      // Clear timer ref if scheduling failed so callback isn't leaked.
      cb->Notify(nullptr);
    }
  }

  return abortSignal.forget();
}

already_AddRefed<AbortSignal>
AbortSignal::Any(GlobalObject& aGlobal,
                 const Sequence<OwningNonNull<AbortSignal>>& aSignals)
{
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(aGlobal.GetAsSupports());
  for (uint32_t i = 0; i < aSignals.Length(); ++i) {
    if (aSignals[i]->Aborted()) {
      RefPtr<AbortSignal> aborted = new AbortSignal(global, true);
      return aborted.forget();
    }
  }
  RefPtr<AbortSignal> composite = new AbortSignal(global, false);
  for (uint32_t i = 0; i < aSignals.Length(); ++i) {
    composite->mCompositeFollowers.AppendElement(
      MakeUnique<CompositeFollower>(composite, aSignals[i]));
  }
  return composite.forget();
}

void
AbortSignal::ThrowIfAborted(ErrorResult& aRv) const
{
  if (mAborted) {
    aRv.Throw(NS_ERROR_DOM_ABORT_ERR);
  }
}

bool
AbortSignal::Aborted() const
{
  return mAborted;
}

void
AbortSignal::Abort()
{
  // Re-entrancy guard
  if (mAborted) {
    return;
  }
  mAborted = true;

  // We might be deleted as a result of aborting a follower, so ensure we
  // stay alive until all followers have been aborted.
  RefPtr<AbortSignal> pinThis = this;

  // Release any source signal subscriptions now that we are aborted.
  mCompositeFollowers.Clear();

  // Let's inform the followers. Swap out mFollowers so re-entrant
  // RemoveFollower() calls (e.g. from CompositeFollower) cannot skip entries.
  nsTArray<Follower*> followers;
  followers.SwapElements(mFollowers);
  for (uint32_t i = 0; i < followers.Length(); ++i) {
    followers[i]->Aborted();
  }

  EventInit init;
  init.mBubbles = false;
  init.mCancelable = false;

  RefPtr<Event> event =
    Event::Constructor(this, NS_LITERAL_STRING("abort"), init);
  event->SetTrusted(true);

  bool dummy;
  DispatchEvent(event, &dummy);
}

void
AbortSignal::AddFollower(AbortSignal::Follower* aFollower)
{
  MOZ_DIAGNOSTIC_ASSERT(aFollower);
  if (!mFollowers.Contains(aFollower)) {
    mFollowers.AppendElement(aFollower);
  }
}

void
AbortSignal::RemoveFollower(AbortSignal::Follower* aFollower)
{
  MOZ_DIAGNOSTIC_ASSERT(aFollower);
  mFollowers.RemoveElement(aFollower);
}

// AbortSignal::Follower
// ----------------------------------------------------------------------------

AbortSignal::Follower::~Follower()
{
  Unfollow();
}

void
AbortSignal::Follower::Follow(AbortSignal* aSignal)
{
  MOZ_DIAGNOSTIC_ASSERT(aSignal);

  Unfollow();

  mFollowingSignal = aSignal;
  aSignal->AddFollower(this);
}

void
AbortSignal::Follower::Unfollow()
{
  if (mFollowingSignal) {
    mFollowingSignal->RemoveFollower(this);
    mFollowingSignal = nullptr;
  }
}

} // dom namespace
} // mozilla namespace
