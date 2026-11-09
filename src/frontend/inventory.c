#include "gui_view.h"
#include "gui.h"
#include "inventory.h"
#include "ui_font.h"

#include <stddef.h>

enum {
    INVENTORY_COLUMNS = 8,
    INVENTORY_ROWS = 4,
    INVENTORY_SLOT_SIZE = 58,
    INVENTORY_SLOT_GAP = 4,
    INVENTORY_PANEL_PADDING = 18,
    INVENTORY_HEADER_HEIGHT = 52,
    INVENTORY_FOOTER_HEIGHT = 32
};

_Static_assert(INVENTORY_COLUMNS*INVENTORY_ROWS == INVENTORY_SLOT_COUNT,
        "inventory grid must match slot capacity");

typedef struct {
    BlockId id;
    const char *path;
} BlockTexture;

typedef struct {
    Rectangle panel;
    float grid_x;
    float grid_y;
} InventoryLayout;

static const BlockTexture block_textures[] = {
    {BLOCK_IRON, "res/textures/item/iron_block.png"},
    {BLOCK_GLASS, "res/textures/item/glass.png"},
    {BLOCK_REDSTONE_BLOCK, "res/textures/item/redstone_block.png"},
    {BLOCK_REDSTONE_WIRE, "res/textures/item/redstone_wire.png"},
    {BLOCK_REDSTONE_TORCH, "res/textures/item/redstone_torch.png"},
    {BLOCK_REDSTONE_REPEATER, "res/textures/item/redstone_repeater.png"},
    {BLOCK_REDSTONE_COMPARATOR, "res/textures/item/redstone_comparator.png"},
    {BLOCK_POWERED_RAIL, "res/textures/item/powered_rail.png"},
    {BLOCK_ACTIVATOR_RAIL, "res/textures/item/activator_rail.png"},
    {BLOCK_OBSERVER, "res/textures/item/observer.png"},
    {BLOCK_PISTON, "res/textures/item/piston.png"},
    {BLOCK_STICKY_PISTON, "res/textures/item/sticky_piston.png"},
    {BLOCK_BARREL, "res/textures/item/barrel.png"},
    {BLOCK_SLIME, "res/textures/item/slime_block.png"},
    {BLOCK_HONEY, "res/textures/item/honey_block.png"},
    {BLOCK_NOTE_BLOCK, "res/textures/item/note_block.png"},
    {BLOCK_TARGET, "res/textures/item/target_block.png"},
    {BLOCK_HOPPER, "res/textures/item/hopper.png"},
    {BLOCK_DROPPER, "res/textures/item/dropper.png"},
    {BLOCK_REDSTONE_LAMP, "res/textures/item/redstone_lamp.png"},
    {BLOCK_LEVER, "res/textures/block/lever0.png"}
};

static InventoryLayout inventory_layout(void)
{
    const int grid_width =
        INVENTORY_COLUMNS*INVENTORY_SLOT_SIZE +
        (INVENTORY_COLUMNS - 1)*INVENTORY_SLOT_GAP;
    const int grid_height =
        INVENTORY_ROWS*INVENTORY_SLOT_SIZE +
        (INVENTORY_ROWS - 1)*INVENTORY_SLOT_GAP;
    const int panel_width = grid_width + 2*INVENTORY_PANEL_PADDING;
    const int panel_height = INVENTORY_PANEL_PADDING +
        INVENTORY_HEADER_HEIGHT + grid_height +
        INVENTORY_FOOTER_HEIGHT + INVENTORY_PANEL_PADDING;
    InventoryLayout layout = {
        {
            (gui_view_width() - panel_width)*0.5f,
            (gui_view_height() - 80 - panel_height)*0.5f,
            (float)panel_width,
            (float)panel_height
        },
        0.0f,
        0.0f
    };
    layout.grid_x = layout.panel.x + INVENTORY_PANEL_PADDING;
    layout.grid_y = layout.panel.y + INVENTORY_PANEL_PADDING +
                    INVENTORY_HEADER_HEIGHT;
    return layout;
}

static Rectangle inventory_slot_rectangle(InventoryLayout layout, int index)
{
    int column = index%INVENTORY_COLUMNS;
    int row = index/INVENTORY_COLUMNS;
    return (Rectangle){
        layout.grid_x + column*(INVENTORY_SLOT_SIZE + INVENTORY_SLOT_GAP),
        layout.grid_y + row*(INVENTORY_SLOT_SIZE + INVENTORY_SLOT_GAP),
        INVENTORY_SLOT_SIZE,
        INVENTORY_SLOT_SIZE
    };
}

bool inventory_init(Inventory *inventory)
{
    if (!inventory) return false;
    *inventory = (Inventory){0};

    size_t count = sizeof(block_textures)/sizeof(block_textures[0]);
    if (count + 2 > INVENTORY_SLOT_COUNT) {
        TraceLog(LOG_ERROR, "INVENTORY: Too many item textures (%d > %d)",
                 (int)count + 2, INVENTORY_SLOT_COUNT);
        return false;
    }

    for (size_t index = 0; index < count; ++index) {
        BlockTexture item = block_textures[index];
        Texture2D texture = LoadTexture(gui_asset_path(item.path));
        if (!texture.id) {
            TraceLog(LOG_ERROR, "INVENTORY: Cannot load %s", item.path);
            inventory_unload(inventory);
            return false;
        }
        SetTextureFilter(texture, TEXTURE_FILTER_POINT);
        inventory->block_textures[item.id] = texture;
        inventory->slots[index] = (InventorySlot){
            .kind = INVENTORY_BLOCK, .block = item.id
        };
    }
    Image potato = LoadImage(gui_asset_path("res/textures/item/poisonous_potato.png"));
    if (!potato.data) {
        inventory_unload(inventory);
        return false;
    }
    ImageAlphaCrop(&potato, 0.0f);
    Texture2D texture = LoadTextureFromImage(potato);
    UnloadImage(potato);
    if (!texture.id) {
        inventory_unload(inventory);
        return false;
    }
    SetTextureFilter(texture, TEXTURE_FILTER_POINT);
    inventory->item_textures[ITEM_POISONOUS_POTATO] = texture;
    inventory->slots[count] = (InventorySlot){
        .kind = INVENTORY_ITEM,
        .stack = {ITEM_POISONOUS_POTATO, 1,
                  item_stack_limit(ITEM_POISONOUS_POTATO)}
    };
    Image axe = LoadImage(gui_asset_path("res/textures/item/wooden_axe.png"));
    if (!axe.data) { inventory_unload(inventory); return false; }
    ImageAlphaCrop(&axe, 0.0f);
    texture = LoadTextureFromImage(axe);
    UnloadImage(axe);
    if (!texture.id) { inventory_unload(inventory); return false; }
    SetTextureFilter(texture, TEXTURE_FILTER_POINT);
    inventory->tool_textures[TOOL_WOODEN_AXE] = texture;
    inventory->slots[count + 1] = (InventorySlot){
        .kind = INVENTORY_TOOL, .tool = TOOL_WOODEN_AXE
    };
    inventory->count = (int)count + 2;
    return true;
}

void inventory_unload(Inventory *inventory)
{
    if (!inventory) return;
    for (int id = 0; id < BLOCK_COUNT; ++id) {
        if (inventory->block_textures[id].id)
            UnloadTexture(inventory->block_textures[id]);
    }
    for (int id = 0; id < ITEM_COUNT; ++id) {
        if (inventory->item_textures[id].id)
            UnloadTexture(inventory->item_textures[id]);
    }
    for (int id = 0; id < TOOL_COUNT; ++id) {
        if (inventory->tool_textures[id].id)
            UnloadTexture(inventory->tool_textures[id]);
    }
    *inventory = (Inventory){0};
}

static void draw_icon(Texture2D texture, Rectangle slot)
{
    if (!texture.id) return;

    const float padding = 5.0f;
    float scale_x = (slot.width - 2.0f*padding)/(float)texture.width;
    float scale_y = (slot.height - 2.0f*padding)/(float)texture.height;
    float scale = scale_x < scale_y ? scale_x : scale_y;
    float width = texture.width*scale;
    float height = texture.height*scale;
    Rectangle source = {0, 0, (float)texture.width, (float)texture.height};
    Rectangle destination = {
        slot.x + (slot.width - width)*0.5f,
        slot.y + (slot.height - height)*0.5f,
        width,
        height
    };
    DrawTexturePro(texture, source, destination, (Vector2){0, 0}, 0.0f, WHITE);
}

void inventory_draw_stack(const Inventory *inventory, ItemStack stack,
                           Rectangle slot)
{
    if (!inventory || !stack.count) return;
    Texture2D texture = stack.item < ITEM_COUNT ?
                        inventory->item_textures[stack.item] : (Texture2D){0};
    if (texture.id) draw_icon(texture, slot);
    else ui_text_draw("?", (int)slot.x + 18, (int)slot.y + 10, 26, RAYWHITE);
    if (stack.count > 1) {
        const char *count = TextFormat("%u", (unsigned)stack.count);
        int x = (int)(slot.x + slot.width) - ui_text_width(count, 18) - 4;
        int y = (int)(slot.y + slot.height) - 22;
        ui_text_draw(count, x + 1, y + 1, 18, BLACK);
        ui_text_draw(count, x, y, 18, RAYWHITE);
    }
}

void inventory_draw_slot(const Inventory *inventory, InventorySlot contents,
                          Rectangle slot)
{
    if (!inventory) return;
    if (contents.kind == INVENTORY_BLOCK && contents.block > BLOCK_AIR &&
        contents.block < BLOCK_COUNT)
        draw_icon(inventory->block_textures[contents.block], slot);
    else if (contents.kind == INVENTORY_ITEM)
        inventory_draw_stack(inventory, contents.stack, slot);
    else if (contents.kind == INVENTORY_TOOL && contents.tool > TOOL_NONE && contents.tool < TOOL_COUNT)
        draw_icon(inventory->tool_textures[contents.tool], slot);
}

const char *inventory_slot_name(InventorySlot slot)
{
    if (slot.kind == INVENTORY_BLOCK) return block_name(slot.block);
    if (slot.kind == INVENTORY_ITEM) return item_name(slot.stack.item);
    if (slot.kind == INVENTORY_TOOL && slot.tool == TOOL_WOODEN_AXE)
        return "Wooden axe (selection)";
    return "Empty";
}

InventorySlot inventory_update(const Inventory *inventory)
{
    bool left = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
    bool right = IsMouseButtonReleased(MOUSE_BUTTON_RIGHT);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (!inventory || (!left && !(shift && right)))
        return (InventorySlot){0};

    InventoryLayout layout = inventory_layout();
    Vector2 mouse = gui_view_mouse();
    for (int index = 0; index < inventory->count; ++index) {
        Rectangle slot = inventory_slot_rectangle(layout, index);
        if (!CheckCollisionPointRec(mouse, slot)) continue;
        InventorySlot picked = inventory->slots[index];
        if (picked.kind == INVENTORY_ITEM) {
            picked.stack.count = !shift ? 1 :
                                 (left ? picked.stack.limit : picked.stack.limit/2);
            return picked;
        }
        if (left) return picked;
    }
    return (InventorySlot){0};
}

void inventory_draw(const Inventory *inventory, InventorySlot selected)
{
    if (!inventory) return;

    InventoryLayout layout = inventory_layout();
    Vector2 mouse = gui_view_mouse();
    InventorySlot hovered = {0};

    DrawRectangle(0, 0, gui_view_width(), gui_view_height(),
                  (Color){10, 18, 20, 155});
    DrawRectangleRec(layout.panel, (Color){226, 218, 196, 250});
    DrawRectangleLinesEx(layout.panel, 3.0f, (Color){34, 63, 66, 255});

    const char *title = "INVENTORY";
    int title_size = 25;
    ui_text_draw(title,
             (int)(layout.panel.x +
                   (layout.panel.width - ui_text_width(title, title_size))*0.5f),
             (int)layout.panel.y + 17, title_size, (Color){26, 50, 55, 255});

    for (int index = 0; index < INVENTORY_SLOT_COUNT; ++index) {
        Rectangle slot = inventory_slot_rectangle(layout, index);
        bool occupied = index < inventory->count;
        bool under_mouse = CheckCollisionPointRec(mouse, slot);
        InventorySlot contents = occupied ? inventory->slots[index] :
                                            (InventorySlot){0};
        Color fill = under_mouse && occupied ? (Color){102, 116, 112, 255}
                                             : (Color){75, 75, 75, 235};
        DrawRectangleRec(slot, fill);
        DrawRectangleLinesEx(slot, 1.0f, (Color){145, 145, 145, 255});
        if (occupied) inventory_draw_slot(inventory, contents, slot);
        bool same = contents.kind == selected.kind &&
                    ((contents.kind == INVENTORY_BLOCK &&
                      contents.block == selected.block) ||
                     (contents.kind == INVENTORY_ITEM &&
                      contents.stack.item == selected.stack.item) ||
                     (contents.kind == INVENTORY_TOOL &&
                      contents.tool == selected.tool));
        if (same)
            DrawRectangleLinesEx(slot, 3.0f, (Color){244, 190, 88, 255});
        if (under_mouse && occupied) hovered = contents;
    }

    if (hovered.kind != INVENTORY_EMPTY) {
        const char *label = inventory_slot_name(hovered);
        int label_size = 19;
        int label_y = (int)(layout.grid_y +
            INVENTORY_ROWS*INVENTORY_SLOT_SIZE +
            (INVENTORY_ROWS - 1)*INVENTORY_SLOT_GAP + 8);
        ui_text_draw(label,
                 (int)(layout.panel.x +
                       (layout.panel.width - ui_text_width(label, label_size))*0.5f),
                 label_y, label_size, (Color){26, 50, 55, 255});
    }
}
