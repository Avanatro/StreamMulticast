// Pure lock rules of the vertical editor. No OBS calls.
#include "TestRunner.hpp"
#include "core/ItemActions.hpp"
#include "core/LockPolicy.hpp"

using namespace smulti;
using preview_edit::Item;

static Item item(int64_t id, bool locked)
{
	Item it;
	it.id = id;
	it.locked = locked;
	it.pos = {static_cast<float>(id) * 10, 0};
	it.source = {100, 100};
	return it;
}

SMULTI_TEST(vertical_lock_drag_skips_locked)
{
	const std::vector<Item> items = {item(1, true), item(2, false)};
	EXPECT_TRUE(!lock_policy::can_edit(items, 1));
	EXPECT_TRUE(lock_policy::can_edit(items, 2));
	EXPECT_TRUE(!lock_policy::can_edit(items, 99)); // unknown id: nothing to write
	EXPECT_TRUE(lock_policy::editable_ids(items, {1}).empty());
	// a locked item is still found by the selection hit test (it can be selected and get a context menu)
	EXPECT_EQ(preview_edit::hit_item_selectable(items, {15, 50}), int64_t(1));
	// ... but never by the drag hit test
	EXPECT_EQ(preview_edit::hit_item(items, {15, 50}), int64_t(0));
}

SMULTI_TEST(vertical_lock_multiselect_moves_unlocked_only)
{
	const std::vector<Item> items = {item(1, false), item(2, true), item(3, false)};
	const auto ids = lock_policy::editable_ids(items, {3, 2, 1, 1, 99});
	EXPECT_EQ(ids.size(), size_t(2));
	EXPECT_EQ(ids[0], int64_t(3));
	EXPECT_EQ(ids[1], int64_t(1));
}

SMULTI_TEST(vertical_lock_nudge_ignored_when_locked)
{
	std::vector<Item> items = {item(5, true)};
	EXPECT_TRUE(lock_policy::editable_ids(items, {5}).empty());
	items[0].locked = false; // unlocked again: the same nudge goes through
	const auto ids = lock_policy::editable_ids(items, {5});
	EXPECT_EQ(ids.size(), size_t(1));
	EXPECT_EQ(ids[0], int64_t(5));
}

SMULTI_TEST(vertical_lock_copy_transform_skips_locked_targets)
{
	using item_actions::SceneItemRef;
	const std::vector<SceneItemRef> c = {{"u-a", "A", 1, "cam", false},
					     {"u-b", "B", 2, "cam", true},
					     {"u-c", "C", 3, "cam", true},
					     {"u-d", "D", 4, "cam", false}};
	const auto plan = item_actions::plan_copy_transform("cam", "u-a", c);
	EXPECT_EQ(plan.targets.size(), size_t(1));
	EXPECT_EQ(plan.targets[0].scene_uuid, std::string("u-d"));
	EXPECT_EQ(plan.skipped.size(), size_t(2));
	EXPECT_EQ(item_actions::skipped_locked_count(plan), size_t(2));
}

SMULTI_TEST(vertical_lock_all_unlock_all_flags)
{
	const std::vector<Item> items = {item(1, false), item(2, true), item(3, false)};
	const auto lock = lock_policy::lock_all_changes(items, true);
	EXPECT_EQ(lock.size(), size_t(2));
	EXPECT_EQ(lock[0], int64_t(1));
	EXPECT_EQ(lock[1], int64_t(3));
	const auto unlock = lock_policy::lock_all_changes(items, false);
	EXPECT_EQ(unlock.size(), size_t(1));
	EXPECT_EQ(unlock[0], int64_t(2));
	// already in the wanted state: nothing to change
	EXPECT_TRUE(lock_policy::lock_all_changes({item(1, true)}, true).empty());
	EXPECT_TRUE(lock_policy::lock_all_changes({}, false).empty());
}
