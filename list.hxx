#include <cstdio>
#include <cstdlib>

struct item {
	void* data;
	struct item* prev;
	struct item* next;
};
void movetohead(struct item** mainlist, struct item* item)
{ // Move element in item to the head of list mainlist.
	if (nullptr == item || nullptr == mainlist || nullptr == *mainlist) return;
	/* item is NULL or we're already at head. Do nothing. */
	if (*mainlist == item) return;
	/* Braid together the list where we are now. */
	if (nullptr != item->prev) item->prev->next = item->next;

	if (nullptr != item->next) item->next->prev = item->prev;
	/* Now we'at head, so no one before us. */
	item->prev = nullptr;
	/* Old head is our next. */
	item->next = *mainlist;
	/* Old head needs to know about us. */
	item->next->prev = item;
	/* Remember the new head. */
	*mainlist = item;
}

auto additem(struct item** mainlist) -> struct item*
{ // Create space for a new item and add it to the head of mainlist.
  // Returns item or NULL if out of memory.
	struct item* item;

	if (nullptr == (item = (struct item*)malloc(sizeof(struct item)))) return nullptr;
	/* First in the list. */
	if (nullptr == *mainlist)
		item->prev = item->next = nullptr;
	else {
		/* Add to beginning of list. */
		item->next = *mainlist;
		item->next->prev = item;
		item->prev = nullptr;
	}
	*mainlist = item;
	return item;
}

void delitem(struct item** mainlist, struct item* item)
{
	struct item* ml = *mainlist;

	if (nullptr == mainlist || nullptr == *mainlist || nullptr == item) return;
	/* First entry was removed. Remember the next one instead. */
	if (item == *mainlist) {
		*mainlist = ml->next;
		if (item->next != nullptr) item->next->prev = nullptr;
	} else {
		item->prev->next = item->next;
		/* This is not the last item in the list. */
		if (nullptr != item->next) item->next->prev = item->prev;
	}
	free(item);
}

void freeitem(struct item** list, int* stored, struct item* item)
{
	if (nullptr == list || nullptr == *list || nullptr == item) return;

	if (nullptr != item->data) {
		free(item->data);
		item->data = nullptr;
	}
	delitem(list, item);

	if (nullptr != stored) (*stored)--;
}

void delallitems(struct item** list, int* stored)
{ // Delete all elements in list and free memory resources.
	struct item* item;
	struct item* next;

	for (item = *list; item != nullptr; item = next) {
		next = item->next;
		free(item->data);
		delitem(list, item);
	}

	if (nullptr != stored) (*stored) = 0;
}

void listitems(struct item* mainlist)
{
	struct item* item;
	int i;
	for (item = mainlist, i = 1; item != nullptr; item = item->next, i++)
		printf("item #%d (stored at %p).\n", i, (void*)item);
}
