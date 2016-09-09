// Copyright 2010-2015 RethinkDB, all rights reserved.
#include "clustering/administration/auth/users_artificial_table_backend.hpp"

namespace auth {

users_artificial_table_backend_t::users_artificial_table_backend_t(
        std::shared_ptr<semilattice_readwrite_view_t<auth_semilattice_metadata_t>>
            auth_semilattice_view,
        std::shared_ptr<semilattice_read_view_t<cluster_semilattice_metadata_t>>
            cluster_semilattice_view)
    : base_artificial_table_backend_t(
        auth_semilattice_view,
        cluster_semilattice_view) {
}

ql::datum_t user_to_datum(username_t const &username, user_t const &user) {
    ql::datum_object_builder_t builder;
    builder.overwrite("id", ql::datum_t(datum_string_t(username.to_string())));
    builder.overwrite("password", ql::datum_t::boolean(!user.get_password().is_empty()));
    return std::move(builder).to_datum();
}

bool users_artificial_table_backend_t::read_all_rows_as_vector(
        UNUSED signal_t *interruptor,
        std::vector<ql::datum_t> *rows_out,
        UNUSED admin_err_t *error_out) {
    rows_out->clear();
    on_thread_t on_thread(home_thread());

    auth_semilattice_metadata_t auth_metadata = m_auth_semilattice_view->get();
    for (auto const &user : auth_metadata.m_users) {
        if (!static_cast<bool>(user.second.get_ref())) {
            continue;
        }

        rows_out->push_back(user_to_datum(user.first, user.second.get_ref().get()));
    }

    return true;
}

bool users_artificial_table_backend_t::read_row(
        ql::datum_t primary_key,
        UNUSED signal_t *interruptor,
        ql::datum_t *row_out,
        UNUSED admin_err_t *error_out) {
    *row_out = ql::datum_t();
    on_thread_t on_thread(home_thread());

    if (primary_key.get_type() == ql::datum_t::R_STR) {
        username_t username(primary_key.as_str().to_std());

        auth_semilattice_metadata_t auth_metadata = m_auth_semilattice_view->get();
        auto user = auth_metadata.m_users.find(username);
        if (user != auth_metadata.m_users.end() &&
                static_cast<bool>(user->second.get_ref())) {
            *row_out = user_to_datum(user->first, user->second.get_ref().get());
        }
    }

    return true;
}

bool users_artificial_table_backend_t::write_row(
        ql::datum_t primary_key,
        bool pkey_was_autogenerated,
        ql::datum_t *new_value_inout,
        UNUSED signal_t *interruptor,
        admin_err_t *error_out) {
    on_thread_t on_thread(home_thread());

    if (primary_key.get_type() != ql::datum_t::R_STR || pkey_was_autogenerated) {
        *error_out = admin_err_t{
            "Expected a username as the primary key, got " + primary_key.print() + ".",
            query_state_t::FAILED};
        return false;
    }

    username_t username(primary_key.as_str().to_std());

    if (new_value_inout->has()) {
        try {
            auth_semilattice_metadata_t auth_metadata = m_auth_semilattice_view->get();
            auto user = auth_metadata.m_users.find(username);
            if (user != auth_metadata.m_users.end() &&
                    static_cast<bool>(user->second.get_ref())) {
                user->second.apply_write([&](boost::optional<auth::user_t> *inner_user) {
                    inner_user->get().merge(*new_value_inout);
                });
            } else {
                auth_metadata.m_users[username].set(user_t(*new_value_inout));
            }
            m_auth_semilattice_view->join(auth_metadata);
        } catch(admin_op_exc_t const &admin_op_exc) {
            *error_out = admin_op_exc.to_admin_err();
            return false;
        }
    } else {
        if (username.is_admin()) {
            *error_out = admin_err_t{
                "The user `" + username.to_string() + "` can't be deleted.",
                query_state_t::FAILED};
            return false;
        }

        auth_semilattice_metadata_t auth_metadata = m_auth_semilattice_view->get();
        auto user = auth_metadata.m_users.find(username);
        if (user != auth_metadata.m_users.end() &&
                static_cast<bool>(user->second.get_ref())) {
            user->second.set(boost::none);
        } else {
            *error_out = admin_err_t{
                "User `" + username.to_string() + "` not found.",
                query_state_t::FAILED};
            return false;
        }
        m_auth_semilattice_view->join(auth_metadata);
    }

    return true;
}

}  // namespace auth
