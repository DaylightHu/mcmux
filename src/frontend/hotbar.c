#include "gui_view.h"
#include "hotbar.h"
#include "app_config.h"
#include <stdio.h>

enum {
    HOTBAR_SLOT_SIZE = 48,
    HOTBAR_PADDING = 4,
    HOTBAR_BOTTOM_MARGIN = 12
};

static Rectangle hotbar_slot_rectangle(int index)
{
    int bar_width = HOTBAR_SLOT_COUNT*HOTBAR_SLOT_SIZE + 2*HOTBAR_PADDING;
    int bar_x = (gui_view_width() - bar_width)/2;
    int bar_y = gui_view_height() -
                (HOTBAR_SLOT_SIZE + 2*HOTBAR_PADDING) - HOTBAR_BOTTOM_MARGIN;
    return (Rectangle){
        (float)(bar_x + HOTBAR_PADDING + index*HOTBAR_SLOT_SIZE),
        (float)(bar_y + HOTBAR_PADDING),
        HOTBAR_SLOT_SIZE,
        HOTBAR_SLOT_SIZE
    };
}

int hotbar_slot_at(Vector2 position)
{
    for (int slot = 0; slot < HOTBAR_SLOT_COUNT; ++slot) {
        if (CheckCollisionPointRec(position, hotbar_slot_rectangle(slot)))
            return slot;
    }
    return -1;
}

bool hotbar_init(Hotbar *hotbar, const Inventory *inventory)
{
    if (!hotbar || !inventory) return false;
    *hotbar = (Hotbar){0};
    int count = inventory->count < HOTBAR_SLOT_COUNT ?
                inventory->count : HOTBAR_SLOT_COUNT;
    for (int index = 0; index < count; ++index)
        hotbar->slots[index] = inventory->slots[index];
    hotbar->selected_slot = 0;
    return true;
}

void hotbar_unload(Hotbar *hotbar)
{
    if (!hotbar) return;
    *hotbar = (Hotbar){0};
}

void hotbar_update(Hotbar *hotbar)
{
    if (!hotbar) return;

    for (int slot=0;slot<HOTBAR_SLOT_COUNT;++slot) {
        char action[16]; snprintf(action,sizeof(action),"slot_%d",slot+1);
        if (app_input_world(action,false)) { hotbar->selected_slot=slot; return; }
    }

    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        int slot = hotbar_slot_at(gui_view_mouse());
        if (slot >= 0) {
            hotbar->selected_slot = slot;
            return;
        }
    }

    float wheel = GetMouseWheelMove();
    if (wheel > 0.0f) {
        hotbar->selected_slot =
            (hotbar->selected_slot + HOTBAR_SLOT_COUNT - 1)%HOTBAR_SLOT_COUNT;
    } else if (wheel < 0.0f) {
        hotbar->selected_slot =
            (hotbar->selected_slot + 1)%HOTBAR_SLOT_COUNT;
    }
}

InventorySlot hotbar_selected(const Hotbar *hotbar)
{
    if (!hotbar || hotbar->selected_slot < 0 ||
        hotbar->selected_slot >= HOTBAR_SLOT_COUNT)
        return (InventorySlot){0};
    return hotbar->slots[hotbar->selected_slot];
}

BlockId hotbar_selected_block(const Hotbar *hotbar)
{
    InventorySlot slot = hotbar_selected(hotbar);
    return slot.kind == INVENTORY_BLOCK ? slot.block : BLOCK_AIR;
}

bool hotbar_set_selected(Hotbar *hotbar, InventorySlot slot)
{
    if (!hotbar || hotbar->selected_slot < 0 ||
        hotbar->selected_slot >= HOTBAR_SLOT_COUNT) return false;
    if (slot.kind == INVENTORY_BLOCK) {
        if (slot.block <= BLOCK_AIR || slot.block >= BLOCK_INTERNAL_FIRST)
            return false;
    } else if (slot.kind == INVENTORY_ITEM) {
        if (!slot.stack.count || !item_stack_valid(slot.stack)) return false;
    } else if (slot.kind == INVENTORY_TOOL) {
        if (slot.tool <= TOOL_NONE || slot.tool >= TOOL_COUNT) return false;
    } else if (slot.kind != INVENTORY_EMPTY) return false;
    hotbar->slots[hotbar->selected_slot] = slot;
    return true;
}

bool hotbar_set_selected_block(Hotbar *hotbar, BlockId id)
{
    return hotbar_set_selected(hotbar, (InventorySlot){
        .kind = INVENTORY_BLOCK, .block = id
    });
}

bool hotbar_pick_block(Hotbar *hotbar, BlockId id)
{
    if (!hotbar || id <= BLOCK_AIR || id >= BLOCK_INTERNAL_FIRST)
        return false;

    for (int slot = 0; slot < HOTBAR_SLOT_COUNT; ++slot) {
        InventorySlot contents = hotbar->slots[slot];
        if (contents.kind == INVENTORY_BLOCK && contents.block == id) {
            hotbar->selected_slot = slot;
            return true;
        }
    }
    return hotbar_set_selected_block(hotbar, id);
}

bool hotbar_pick(Hotbar *hotbar, InventorySlot picked)
{
    InventorySlot selected = hotbar_selected(hotbar);
    if (picked.kind == INVENTORY_ITEM && selected.kind == INVENTORY_ITEM &&
        picked.stack.item == selected.stack.item &&
        picked.stack.limit == selected.stack.limit) {
        if (!item_stack_transfer(&selected.stack, &picked.stack,
                                  picked.stack.count)) return false;
        return hotbar_set_selected(hotbar, selected);
    }
    return picked.kind != INVENTORY_EMPTY && hotbar_set_selected(hotbar, picked);
}

void hotbar_draw(const Hotbar *hotbar, const Inventory *inventory)
{
    if (!hotbar || !inventory) return;

    const int bar_width = HOTBAR_SLOT_COUNT*HOTBAR_SLOT_SIZE + 2*HOTBAR_PADDING;
    const int bar_height = HOTBAR_SLOT_SIZE + 2*HOTBAR_PADDING;
    int bar_x = (gui_view_width() - bar_width)/2;
    int bar_y = gui_view_height() - bar_height - HOTBAR_BOTTOM_MARGIN;

    DrawRectangle(bar_x, bar_y, bar_width, bar_height,
                  (Color){20, 20, 20, 210});

    for (int slot_index = 0; slot_index < HOTBAR_SLOT_COUNT; ++slot_index) {
        Rectangle slot = hotbar_slot_rectangle(slot_index);
        DrawRectangleRec(slot, (Color){75, 75, 75, 190});

        inventory_draw_slot(inventory, hotbar->slots[slot_index], slot);

        DrawRectangleLinesEx(slot, 1.0f, (Color){145, 145, 145, 255});
        if (slot_index == hotbar->selected_slot) {
            Rectangle selection = {
                slot.x - 2.0f, slot.y - 2.0f,
                slot.width + 4.0f, slot.height + 4.0f
            };
            DrawRectangleLinesEx(selection, 3.0f, RAYWHITE);
        }
    }
}

