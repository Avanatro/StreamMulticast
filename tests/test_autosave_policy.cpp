// Pure debounce rule for the scene collection autosave. No OBS calls, no clock, no Qt.
#include "TestRunner.hpp"
#include "core/AutosavePolicy.hpp"

using smulti::AutosavePolicy;

SMULTI_TEST(autosave_debounces_burst_into_one_save)
{
	AutosavePolicy p;
	for (int64_t t = 0; t <= 1500; t += 500)
		p.request(t);
	/* Last request at 1500: the single save is due 2000 ms after it, never earlier. */
	EXPECT_TRUE(!p.due(1500));
	EXPECT_TRUE(!p.due(3499));
	EXPECT_TRUE(p.due(3500));
	EXPECT_TRUE(!p.due(3501)); // one burst, one save
	EXPECT_TRUE(!p.pending());
}

SMULTI_TEST(autosave_fires_after_delay)
{
	AutosavePolicy p;
	EXPECT_TRUE(!p.due(0)); // nothing requested
	p.request(1000);
	EXPECT_TRUE(p.pending());
	EXPECT_EQ(p.deadline_ms(), static_cast<int64_t>(1000 + smulti::kAutosaveDelayMs));
	EXPECT_TRUE(!p.due(2999));
	EXPECT_TRUE(p.due(3000));
	/* A later request arms a fresh cycle. */
	p.request(5000);
	EXPECT_TRUE(!p.due(6999));
	EXPECT_TRUE(p.due(7000));
}

SMULTI_TEST(autosave_blocked_drops_requests)
{
	AutosavePolicy p;
	p.set_blocked(true);
	p.request(0);
	EXPECT_TRUE(!p.pending());
	EXPECT_TRUE(!p.due(100000));
	p.set_blocked(false);
	EXPECT_TRUE(!p.due(100000)); // the dropped request does not come back
	p.request(200000);
	EXPECT_TRUE(p.due(202000)); // unblocked: works again
}

SMULTI_TEST(autosave_unblock_does_not_fire_stale_request)
{
	AutosavePolicy p;
	p.request(0);
	EXPECT_TRUE(p.pending());
	p.set_blocked(true); // collection switch: the pending deadline is cancelled
	EXPECT_TRUE(!p.pending());
	p.set_blocked(false);
	EXPECT_TRUE(!p.pending());
	EXPECT_TRUE(!p.due(5000));
}

SMULTI_TEST(autosave_max_wait_caps_continuous_edits)
{
	AutosavePolicy p;
	/* An edit every 500 ms keeps pushing the quiet deadline, but never past first request + 10 s. */
	for (int64_t t = 0; t <= 9500; t += 500) {
		p.request(t);
		EXPECT_TRUE(!p.due(t));
	}
	EXPECT_EQ(p.deadline_ms(), static_cast<int64_t>(smulti::kAutosaveMaxWaitMs));
	EXPECT_TRUE(p.due(10000));
	/* The cap restarts with the next burst. */
	p.request(10100);
	EXPECT_EQ(p.deadline_ms(), static_cast<int64_t>(10100 + smulti::kAutosaveDelayMs));
}

SMULTI_TEST(autosave_due_fires_once)
{
	AutosavePolicy p;
	p.request(0);
	EXPECT_TRUE(p.due(2000));
	EXPECT_TRUE(!p.due(2000));
	EXPECT_TRUE(!p.due(900000));
	EXPECT_TRUE(!p.pending());
}
