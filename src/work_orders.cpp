#include "work_orders.h"

#include "json_util.h"
#include "lua_bridge.h"

#include <string>

namespace dfcapture {
namespace {

void set_no_store_json(httplib::Response& res, const std::string& json) {
    res.set_header("Cache-Control", "no-store");
    res.set_content(json, "application/json; charset=utf-8");
}

void text_error(httplib::Response& res, int status, const std::string& message) {
    res.status = status;
    res.set_content(message + "\n", "text/plain; charset=utf-8");
}

void register_json_route(httplib::Server& server, const char* path, const char* lua_fn,
                         const char* failure_prefix) {
    server.Get(path, [lua_fn, failure_prefix](const httplib::Request&, httplib::Response& res) {
        std::string err;
        std::string json = order_json_via_lua(lua_fn, &err);
        if (json.empty()) {
            text_error(res, 500, std::string(failure_prefix) + ": " + err);
            return;
        }
        set_no_store_json(res, json);
    });
}

} // namespace

void register_work_order_routes(httplib::Server& server) {
    register_json_route(server, "/orders", "list_orders", "orders failed");
    register_json_route(server, "/order-catalog", "order_catalog", "order catalog failed");
    register_json_route(server, "/order-catalog-shops", "order_catalog_by_shop",
                        "shop catalog failed");
    register_json_route(server, "/order-presets", "order_presets", "order presets failed");
    register_json_route(server, "/condition-targets", "condition_targets",
                        "condition targets failed");
    register_json_route(server, "/order-workshops", "order_workshops",
                        "order workshops failed");

    auto order_create_handler = [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("key")) {
            text_error(res, 400, "missing key");
            return;
        }
        int amount = 1;
        int workshop_id = -1;
        query_int(req, "amount", amount);
        query_int(req, "workshop", workshop_id);
        std::string frequency = req.has_param("frequency")
            ? req.get_param_value("frequency")
            : "OneTime";

        std::string msg;
        std::string err;
        if (!create_order_via_lua(req.get_param_value("key"), amount, frequency,
                                  workshop_id, &msg, &err)) {
            text_error(res, 400, "create order failed: " + err);
            return;
        }
        set_no_store_json(res, "{\"ok\":true,\"msg\":" + json_string(msg) + "}\n");
    };
    server.Get("/order-create", order_create_handler);
    server.Post("/order-create", order_create_handler);

    auto order_import_handler = [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("name")) {
            text_error(res, 400, "missing name");
            return;
        }
        std::string msg;
        std::string err;
        if (!import_order_preset_via_lua(req.get_param_value("name"), &msg, &err)) {
            text_error(res, 400, "import failed: " + err);
            return;
        }
        set_no_store_json(res, "{\"ok\":true,\"msg\":" + json_string(msg) + "}\n");
    };
    server.Get("/order-import", order_import_handler);
    server.Post("/order-import", order_import_handler);

    auto order_cancel_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            text_error(res, 400, "missing id");
            return;
        }
        std::string err;
        if (!cancel_order_via_lua(id, &err)) {
            text_error(res, 400, "cancel failed: " + err);
            return;
        }
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/order-cancel", order_cancel_handler);
    server.Post("/order-cancel", order_cancel_handler);

    auto order_adjust_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        if (!query_int(req, "id", id)) {
            text_error(res, 400, "missing id");
            return;
        }
        int amount = -1;
        query_int(req, "amount", amount);
        std::string frequency = req.has_param("frequency") ? req.get_param_value("frequency") : "";
        std::string err;
        if (!adjust_order_via_lua(id, amount, frequency, &err)) {
            text_error(res, 400, "adjust failed: " + err);
            return;
        }
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/order-adjust", order_adjust_handler);
    server.Post("/order-adjust", order_adjust_handler);

    auto order_cond_item_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        int value = 0;
        if (!query_int(req, "id", id) || !req.has_param("item")) {
            text_error(res, 400, "missing id/item");
            return;
        }
        query_int(req, "value", value);
        std::string compare = req.has_param("compare") ? req.get_param_value("compare") : "AtMost";
        std::string material = req.has_param("material") ? req.get_param_value("material") : "";
        std::string adjective = req.has_param("adjective") ? req.get_param_value("adjective") : "";

        std::string err;
        if (!add_item_condition_via_lua(id, compare, value, req.get_param_value("item"),
                                        material, adjective, &err)) {
            text_error(res, 400, "add condition failed: " + err);
            return;
        }
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/order-condition-item-add", order_cond_item_handler);
    server.Post("/order-condition-item-add", order_cond_item_handler);

    server.Get("/condition-materials", [](const httplib::Request& req, httplib::Response& res) {
        std::string item = req.has_param("item") ? req.get_param_value("item") : "";
        std::string err;
        std::string json = order_json_via_lua_str("condition_materials", item, &err);
        if (json.empty()) {
            text_error(res, 500, "condition materials failed: " + err);
            return;
        }
        set_no_store_json(res, json);
    });

    server.Get("/order-suggested-conditions", [](const httplib::Request& req,
                                                 httplib::Response& res) {
        std::string id = req.has_param("id") ? req.get_param_value("id") : "";
        std::string err;
        std::string json = order_json_via_lua_str("suggested_conditions", id, &err);
        if (json.empty()) {
            text_error(res, 500, "suggested conditions failed: " + err);
            return;
        }
        set_no_store_json(res, json);
    });

    auto order_cond_order_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        int other = -1;
        if (!query_int(req, "id", id) || !query_int(req, "other", other)) {
            text_error(res, 400, "missing id/other");
            return;
        }
        std::string type = req.has_param("type") ? req.get_param_value("type") : "Completed";
        std::string err;
        if (!add_order_condition_via_lua(id, other, type, &err)) {
            text_error(res, 400, "add dependency failed: " + err);
            return;
        }
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/order-condition-order-add", order_cond_order_handler);
    server.Post("/order-condition-order-add", order_cond_order_handler);

    auto order_cond_remove_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        int index = -1;
        if (!query_int(req, "id", id) || !query_int(req, "idx", index)) {
            text_error(res, 400, "missing id/idx");
            return;
        }
        std::string kind = req.has_param("kind") ? req.get_param_value("kind") : "item";
        std::string err;
        if (!remove_condition_via_lua(id, kind, index, &err)) {
            text_error(res, 400, "remove condition failed: " + err);
            return;
        }
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/order-condition-remove", order_cond_remove_handler);
    server.Post("/order-condition-remove", order_cond_remove_handler);

    auto order_maxshops_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        int max_workshops = 0;
        if (!query_int(req, "id", id) || !query_int(req, "max", max_workshops)) {
            text_error(res, 400, "missing id/max");
            return;
        }
        std::string err;
        if (!set_order_max_workshops_via_lua(id, max_workshops, &err)) {
            text_error(res, 400, "update failed: " + err);
            return;
        }
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/order-max-workshops", order_maxshops_handler);
    server.Post("/order-max-workshops", order_maxshops_handler);

    auto order_workshop_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        int workshop = -1;
        if (!query_int(req, "id", id)) {
            text_error(res, 400, "missing id");
            return;
        }
        query_int(req, "workshop", workshop);
        std::string err;
        if (!set_order_workshop_via_lua(id, workshop, &err)) {
            text_error(res, 400, "update failed: " + err);
            return;
        }
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/order-workshop", order_workshop_handler);
    server.Post("/order-workshop", order_workshop_handler);

    auto order_reorder_handler = [](const httplib::Request& req, httplib::Response& res) {
        int id = -1;
        int direction = 0;
        if (!query_int(req, "id", id) || !query_int(req, "dir", direction)) {
            text_error(res, 400, "missing id/dir");
            return;
        }
        std::string err;
        if (!reorder_order_via_lua(id, direction, &err)) {
            text_error(res, 400, "reorder failed: " + err);
            return;
        }
        set_no_store_json(res, "{\"ok\":true}\n");
    };
    server.Get("/order-reorder", order_reorder_handler);
    server.Post("/order-reorder", order_reorder_handler);
}

} // namespace dfcapture
