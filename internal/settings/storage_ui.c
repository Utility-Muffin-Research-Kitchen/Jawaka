#include "catastrophe.h"
#include "catastrophe_widgets.h"

#include "internal/settings/storage_ui.h"
#include "internal/core/log.h"
#include "internal/i18n/i18n.h"

#include <stdio.h>
#include <string.h>

const char *const jw_storage_ui_sources[JW_STORAGE_UI_SOURCE_COUNT] = {
    "launcher_sd",
    "secondary_sd",
};

bool jw_storage_ui_is_read_only(const jw_ipc_storage_status_info *card) {
    return card && card->mounted && strcmp(card->access, "read-only") == 0;
}

bool jw_storage_ui_needs_repair(const jw_ipc_storage_status_info *card) {
    return card && (strcmp(card->repair, "failed") == 0 ||
                    strcmp(card->repair, "pending") == 0);
}

void jw_storage_ui_card_name(const jw_ipc_storage_status_info *card,
                             char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    const char *role = card && strcmp(card->source, "secondary_sd") == 0
                           ? T("second SD card") : T("launcher SD card");
    if (card && card->volume_label[0]) {
        snprintf(out, out_size, "%s (%s)", role, card->volume_label);
    } else {
        snprintf(out, out_size, "%s", role);
    }
}

const char *jw_storage_ui_card_state(const jw_ipc_storage_status_info *card) {
    if (!card || !card->mounted) {
        return T("Not mounted");
    }
    if (strcmp(card->repair, "failed") == 0) {
        return T("Needs repair");
    }
    if (strcmp(card->repair, "pending") == 0 || strcmp(card->repair, "running") == 0) {
        return T("Repair pending");
    }
    if (strcmp(card->access, "read-only") == 0) {
        return T("Read-only");
    }
    if (strcmp(card->access, "unknown") == 0) {
        return T("Unknown");
    }
    return card->busy && strcmp(card->source, "secondary_sd") == 0 ? T("Busy") : T("Mounted");
}

static void jw__storage_ui_message(const char *message) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_A, .label = "OK", .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = message,
        .footer = footer,
        .footer_count = 1,
    };
    cat_confirm_result result;
    (void)cat_confirmation(&opts, &result);
}

static bool jw__storage_ui_confirm(const char *message, const char *cancel,
                                   const char *confirm) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = cancel, .is_confirm = false },
        { .button = CAT_BTN_A, .label = confirm, .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = message,
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result;
    return cat_confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

/* Why repair on the device isn't offered, for a daemon reason key. */
static const char *jw__storage_ui_unavailable_text(const char *reason) {
    if (!reason || !reason[0] || strcmp(reason, "unsupported-platform") == 0) {
        return "";
    }
    if (strcmp(reason, "exfat-not-validated") == 0) {
        return T("exFAT cards can't be repaired on this device yet.");
    }
    if (strcmp(reason, "unsupported-filesystem") == 0) {
        return T("This card's file system can't be repaired on this device.");
    }
    if (strcmp(reason, "tool-missing") == 0 || strcmp(reason, "repair-runner-missing") == 0) {
        return T("Repair isn't installed on this device.");
    }
    if (strcmp(reason, "unknown-identity") == 0) {
        return T("Leaf couldn't identify this card.");
    }
    return "";
}

/* User text for a daemon refusal of a repair request. */
static const char *jw__storage_ui_refusal_text(const char *reason) {
    if (reason && strcmp(reason, "power-required") == 0) {
        return T("Connect your device to power to repair this card.");
    }
    if (reason && strcmp(reason, "busy") == 0) {
        return T("Close the game or app first, then try again.");
    }
    if (reason && strcmp(reason, "internal-storage-unavailable") == 0) {
        return T("Your device's internal storage can't record the repair. Check the card on a computer instead.");
    }
    if (reason && (strcmp(reason, "not-found") == 0 || strcmp(reason, "ambiguous") == 0 ||
                   strcmp(reason, "unknown-identity") == 0)) {
        return T("Leaf couldn't identify this card safely. Check the card on a computer instead.");
    }
    if (reason && strcmp(reason, "write-protected") == 0) {
        return T("Your device can't write to this card, so it can't be repaired here.");
    }
    return T("The repair couldn't be started. Check the card on a computer instead.");
}

bool jw_storage_ui_request_repair(const char *socket_path,
                                  const jw_ipc_storage_status_info *card,
                                  const char *mode) {
    if (!socket_path || !card) {
        return false;
    }
    bool check = mode && strcmp(mode, "check") == 0;
    if (!card->repair_supported) {
        char message[512];
        const char *why = jw__storage_ui_unavailable_text(card->repair_unavailable_reason);
        snprintf(message, sizeof(message), "%s%s%s", why, why[0] ? "\n\n" : "",
                 T("Turn off your device and check the card on a computer."));
        jw__storage_ui_message(message);
        return false;
    }
    if (!card->external_power) {
        jw__storage_ui_message(T("Connect your device to power to repair this card."));
        return false;
    }
    const char *message = check
        ? T("Your device will restart to check this card. If the check passes, the card can be written to again. Keep your device connected to power until the check finishes.")
        : T("Your device will restart to check and repair this card. Damaged files may be shortened, renamed, or recovered under new names. Back up important files on a computer first if you need to recover them. Keep your device connected to power until the check finishes.");
    if (!jw__storage_ui_confirm(message, "Cancel",
                                check ? "Restart and check" : "Restart and repair")) {
        return false;
    }
    char status[256] = "";
    if (jw_ipc_storage_repair_request(socket_path, card->source, check ? "check" : "repair",
                                      status, (int)sizeof(status)) != 0) {
        jw_log_warn("storage: %s request for %s refused: %s", check ? "check" : "repair",
                    card->source, status[0] ? status : "no reply");
        jw__storage_ui_message(jw__storage_ui_refusal_text(status));
        return false;
    }
    return true;
}

bool jw_storage_ui_show_warning(const char *socket_path,
                                const jw_ipc_storage_status_info *card) {
    if (!socket_path || !card) {
        return false;
    }
    char name[128];
    char message[1024];
    jw_storage_ui_card_name(card, name, sizeof(name));
    bool write_protected = strcmp(card->cause, "write-protected") == 0 ||
                           card->block_write_protected;
    if (write_protected) {
        snprintf(message, sizeof(message), "%s\n\n%s\n\n%s %s",
                 T("Your SD card is write-protected"),
                 T("Your device can't write to this card. If you use an adapter with a lock switch, check that it isn't set to lock."),
                 T("Card:"), name);
    } else if (strcmp(card->cause, "filesystem-error") == 0) {
        snprintf(message, sizeof(message), "%s\n\n%s\n\n%s %s",
                 T("Your SD card needs repair"),
                 T("Your SD card is read-only after a file system error. You can't save new files or changes to this card. This can happen after an unexpected shutdown."),
                 T("Card:"), name);
    } else {
        snprintf(message, sizeof(message), "%s\n\n%s\n\n%s %s",
                 T("Your SD card is read-only"),
                 T("Your SD card is read-only. The cause couldn't be determined. You can't save new files or changes to this card."),
                 T("Card:"), name);
    }

    bool offer_repair = card->repair_supported && !write_protected;
    if (!offer_repair && !write_protected) {
        const char *why = jw__storage_ui_unavailable_text(card->repair_unavailable_reason);
        size_t used = strlen(message);
        snprintf(message + used, sizeof(message) - used, "\n\n%s%s%s", why,
                 why[0] ? " " : "",
                 T("To repair it, turn off your device and check the card on a computer."));
    }

    bool repair = false;
    if (offer_repair) {
        repair = jw__storage_ui_confirm(message, "Later", "Repair SD card");
    } else {
        jw__storage_ui_message(message);
    }
    if (jw_ipc_storage_warning_ack(socket_path, card->source) != 0) {
        jw_log_warn("storage: could not acknowledge the warning for %s", card->source);
    }
    return repair && jw_storage_ui_request_repair(socket_path, card, "repair");
}

jw_storage_ui_result_action jw_storage_ui_show_repair_result(
    const char *socket_path, const jw_ipc_storage_status_info *card,
    bool library_writable) {
    if (!socket_path || !card || !card->last_repair_valid) {
        return JW_STORAGE_UI_RESULT_DISMISSED;
    }
    const char *outcome = card->last_repair_outcome;
    bool success = (strcmp(outcome, "repaired") == 0 || strcmp(outcome, "clean") == 0) &&
                   strcmp(card->last_repair_mount_state, "read-write") == 0;
    char message[1024];
    jw_storage_ui_result_action action = JW_STORAGE_UI_RESULT_DISMISSED;
    if (success) {
        snprintf(message, sizeof(message), "%s%s",
                 T("Your card passed the file system check."),
                 strcmp(outcome, "repaired") == 0
                     ? T(" Some damaged files were changed.") : "");
        if (strcmp(outcome, "repaired") == 0) {
            size_t used = strlen(message);
            snprintf(message + used, sizeof(message) - used, "\n\n%s",
                     card->last_repair_reported_changes > 0
                         ? T("The repair log on your device lists the files that were changed. The list may not be complete.")
                         : T("The repair log on your device has the details."));
        }
        if (!library_writable) {
            size_t used = strlen(message);
            snprintf(message + used, sizeof(message) - used, "\n\n%s",
                     T("Your other SD card is still read-only, so your library can't update yet."));
            jw__storage_ui_message(message);
        } else if (jw__storage_ui_confirm(message, "OK", "Scrape missing artwork")) {
            action = JW_STORAGE_UI_RESULT_SCRAPE_MISSING;
        }
    } else {
        const char *hint;
        if (strcmp(outcome, "power-required") == 0) {
            hint = T("Connect your device to power and try again.");
        } else if (strcmp(outcome, "busy") == 0 || strcmp(outcome, "not-found") == 0 ||
                   strcmp(outcome, "ambiguous") == 0 || strcmp(outcome, "unsupported") == 0) {
            hint = T("Leaf couldn't safely check this card, so nothing was changed. Check the card on a computer.");
        } else if (strcmp(outcome, "remount-failed") == 0) {
            hint = T("The card passed the check but couldn't be made writable. Restart your device, then try Check SD card.");
        } else {
            hint = T("Your card is still read-only. Check the card on a computer, then choose Check SD card in Settings.");
        }
        snprintf(message, sizeof(message), "%s\n\n%s",
                 T("The repair did not finish. The log may include changes that were attempted."),
                 hint);
        jw__storage_ui_message(message);
    }
    if (jw_ipc_storage_repair_result_ack(socket_path, card->last_repair_request_id) != 0) {
        jw_log_warn("storage: could not acknowledge repair result %s",
                    card->last_repair_request_id);
    }
    return action;
}

void jw_storage_ui_manage_cards(const char *socket_path, char *status,
                                size_t status_size, bool *unmount_secondary) {
    if (unmount_secondary) {
        *unmount_secondary = false;
    }
    if (!socket_path) {
        return;
    }
    jw_ipc_storage_status_info cards[JW_STORAGE_UI_SOURCE_COUNT];
    char labels[JW_STORAGE_UI_SOURCE_COUNT][192];
    cat_list_item items[JW_STORAGE_UI_SOURCE_COUNT];
    int card_count = 0;
    int card_index[JW_STORAGE_UI_SOURCE_COUNT];
    for (int i = 0; i < JW_STORAGE_UI_SOURCE_COUNT; i++) {
        if (jw_ipc_get_storage_status(socket_path, jw_storage_ui_sources[i],
                                      &cards[i], NULL, 0) != 0) {
            continue;
        }
        char name[128];
        jw_storage_ui_card_name(&cards[i], name, sizeof(name));
        snprintf(labels[card_count], sizeof(labels[card_count]), "%s: %s", name,
                 jw_storage_ui_card_state(&cards[i]));
        items[card_count] = (cat_list_item)CAT_LIST_ITEM(labels[card_count], "card");
        card_index[card_count] = i;
        card_count++;
    }
    if (card_count == 0) {
        if (status && status_size) {
            snprintf(status, status_size, "%s", T("SD card status unavailable"));
        }
        return;
    }

    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Back", .is_confirm = false },
        { .button = CAT_BTN_A, .label = "Choose", .is_confirm = true },
    };
    cat_list_opts opts = cat_list_default_opts(T("SD Cards"), items, card_count);
    opts.footer = footer;
    opts.footer_count = 2;
    cat_list_result picked;
    if (cat_list(&opts, &picked) != CAT_OK || picked.selected_index < 0 ||
        picked.selected_index >= card_count) {
        return;
    }
    const jw_ipc_storage_status_info *card = &cards[card_index[picked.selected_index]];

    enum { ACTION_REPAIR, ACTION_CHECK, ACTION_UNMOUNT };
    cat_list_item actions[3];
    int action_ids[3];
    int action_count = 0;
    bool secondary = strcmp(card->source, "secondary_sd") == 0;
    if (card->mounted && jw_storage_ui_is_read_only(card) &&
        strcmp(card->repair, "pending") != 0) {
        actions[action_count] = (cat_list_item)CAT_LIST_ITEM(T("Repair SD card"), "repair");
        action_ids[action_count++] = ACTION_REPAIR;
    }
    if (card->mounted && strcmp(card->repair, "failed") == 0) {
        actions[action_count] = (cat_list_item)CAT_LIST_ITEM(T("Check SD card"), "check");
        action_ids[action_count++] = ACTION_CHECK;
    }
    if (secondary && card->mounted) {
        actions[action_count] = (cat_list_item)CAT_LIST_ITEM(T("Unmount"), "unmount");
        action_ids[action_count++] = ACTION_UNMOUNT;
    }

    char name[128];
    char title[192];
    jw_storage_ui_card_name(card, name, sizeof(name));
    snprintf(title, sizeof(title), "%s: %s", name, jw_storage_ui_card_state(card));
    if (action_count == 0) {
        char message[512];
        snprintf(message, sizeof(message), "%s\n\n%s", title,
                 card->mounted ? T("This card is working normally.")
                               : T("This card isn't mounted."));
        jw__storage_ui_message(message);
        return;
    }
    cat_list_opts action_opts = cat_list_default_opts(title, actions, action_count);
    action_opts.footer = footer;
    action_opts.footer_count = 2;
    cat_list_result chosen;
    if (cat_list(&action_opts, &chosen) != CAT_OK || chosen.selected_index < 0 ||
        chosen.selected_index >= action_count) {
        return;
    }
    switch (action_ids[chosen.selected_index]) {
        case ACTION_REPAIR:
            if (jw_storage_ui_request_repair(socket_path, card, "repair") && status && status_size) {
                snprintf(status, status_size, "%s", T("Restarting to repair your SD card"));
            }
            break;
        case ACTION_CHECK:
            if (jw_storage_ui_request_repair(socket_path, card, "check") && status && status_size) {
                snprintf(status, status_size, "%s", T("Restarting to check your SD card"));
            }
            break;
        case ACTION_UNMOUNT:
            if (unmount_secondary) {
                *unmount_secondary = true;
            }
            break;
    }
}
