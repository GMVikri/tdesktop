/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/moderate_messages_box.h"

#include "api/api_blocked_peers.h"
#include "api/api_chat_participants.h"
#include "api/api_messages_search.h"
#include "apiwrap.h"
#include "base/event_filter.h"
#include "base/timer.h"
#include "boxes/delete_messages_box.h"
#include "boxes/peers/edit_peer_permissions_box.h"
#include "core/application.h"
#include "core/ui_integration.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_chat_participant_status.h"
#include "data/data_histories.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/stickers/data_custom_emoji.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/boxes/confirm_box.h"
#include "ui/controls/userpic_button.h"
#include "ui/effects/ripple_animation.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rect.h"
#include "ui/rect_part.h"
#include "ui/text/text_utilities.h"
#include "ui/vertical_list.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/expandable_peer_list.h"
#include "ui/widgets/participants_check_view.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/ui_utility.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_window.h"

namespace {

struct ModerateOptions final {
	bool allCanBan = false;
	bool allCanDelete = false;
	Participants participants;
};

ModerateOptions CalculateModerateOptions(const HistoryItemsList &items) {
	Expects(!items.empty());

	auto result = ModerateOptions{
		.allCanBan = true,
		.allCanDelete = true,
	};

	const auto peer = items.front()->history()->peer;
	for (const auto &item : items) {
		if (!result.allCanBan && !result.allCanDelete) {
			return {};
		}
		if (peer != item->history()->peer) {
			return {};
		}
		if (!item->suggestBanReport()) {
			result.allCanBan = false;
		}
		if (!item->suggestDeleteAllReport()) {
			result.allCanDelete = false;
		}
		if (const auto p = item->from()) {
			if (!ranges::contains(result.participants, not_null{ p })) {
				result.participants.push_back(p);
			}
		}
	}
	return result;
}

[[nodiscard]] rpl::producer<int> MessagesCountValue(
		not_null<History*> history,
		not_null<PeerData*> from) {
	return [=](auto consumer) {
		auto lifetime = rpl::lifetime();
		auto search = lifetime.make_state<Api::MessagesSearch>(history);
		consumer.put_next(0);

		search->messagesFounds(
		) | rpl::start_with_next([=](const Api::FoundMessages &found) {
			consumer.put_next_copy(found.total);
		}, lifetime);
		search->searchMessages({ .from = from });

		return lifetime;
	};
}

} // namespace

void CreateModerateMessagesBox(
		not_null<Ui::GenericBox*> box,
		const HistoryItemsList &items,
		Fn<void()> confirmed) {
	using Controller = Ui::ExpandablePeerListController;

	const auto [allCanBan, allCanDelete, participants]
		= CalculateModerateOptions(items);
	const auto inner = box->verticalLayout();

	Assert(!participants.empty());

	const auto confirms = inner->lifetime().make_state<rpl::event_stream<>>();

	const auto isSingle = participants.size() == 1;
	const auto buttonPadding = isSingle
		? QMargins()
		: QMargins(
			0,
			0,
			Ui::ParticipantsCheckView::ComputeSize(
				participants.size()).width(),
			0);

	const auto session = &items.front()->history()->session();
	const auto historyPeerId = items.front()->history()->peer->id;

	using Request = Fn<void(not_null<PeerData*>, not_null<ChannelData*>)>;
	const auto sequentiallyRequest = [=](
			Request request,
			Participants participants) {
		constexpr auto kSmallDelayMs = 5;
		const auto participantIds = ranges::views::all(
			participants
		) | ranges::views::transform([](not_null<PeerData*> peer) {
			return peer->id;
		}) | ranges::to_vector;
		const auto lifetime = std::make_shared<rpl::lifetime>();
		const auto counter = lifetime->make_state<int>(0);
		const auto timer = lifetime->make_state<base::Timer>();
		timer->setCallback(crl::guard(session, [=] {
			if ((*counter) < participantIds.size()) {
				const auto peer = session->data().peer(historyPeerId);
				const auto channel = peer ? peer->asChannel() : nullptr;
				const auto from = session->data().peer(
					participantIds[*counter]);
				if (channel && from) {
					request(from, channel);
				}
				(*counter)++;
			} else {
				lifetime->destroy();
			}
		}));
		timer->callEach(kSmallDelayMs);
	};

	const auto handleConfirmation = [=](
			not_null<Ui::Checkbox*> checkbox,
			not_null<Controller*> controller,
			Request request) {
		confirms->events() | rpl::start_with_next([=] {
			if (checkbox->checked() && controller->collectRequests) {
				sequentiallyRequest(request, controller->collectRequests());
			}
		}, checkbox->lifetime());
	};

	const auto isEnter = [=](not_null<QEvent*> event) {
		if (event->type() == QEvent::KeyPress) {
			if (const auto k = static_cast<QKeyEvent*>(event.get())) {
				return (k->key() == Qt::Key_Enter)
					|| (k->key() == Qt::Key_Return);
			}
		}
		return false;
	};

	base::install_event_filter(box, [=](not_null<QEvent*> event) {
		if (isEnter(event)) {
			box->triggerButton(0);
			return base::EventFilterResult::Cancel;
		}
		return base::EventFilterResult::Continue;
	});

	const auto handleSubmition = [=](not_null<Ui::Checkbox*> checkbox) {
		base::install_event_filter(box, [=](not_null<QEvent*> event) {
			if (!isEnter(event) || !checkbox->checked()) {
				return base::EventFilterResult::Continue;
			}
			box->uiShow()->show(Ui::MakeConfirmBox({
				.text = tr::lng_gigagroup_warning_title(),
				.confirmed = [=](Fn<void()> close) {
					box->triggerButton(0);
					close();
				},
				.confirmText = tr::lng_box_yes(),
				.cancelText = tr::lng_box_no(),
			}));
			return base::EventFilterResult::Cancel;
		});
	};

	Ui::AddSkip(inner);
	const auto title = box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			(items.size() == 1)
				? tr::lng_selected_delete_sure_this()
				: tr::lng_selected_delete_sure(
					lt_count,
					rpl::single(items.size()) | tr::to_count()),
			st::boxLabel));
	Ui::AddSkip(inner);
	Ui::AddSkip(inner);
	Ui::AddSkip(inner);
	{
		const auto report = box->addRow(
			object_ptr<Ui::Checkbox>(
				box,
				tr::lng_report_spam(tr::now),
				false,
				st::defaultBoxCheckbox),
			st::boxRowPadding + buttonPadding);
		const auto controller = box->lifetime().make_state<Controller>(
			Controller::Data{ .participants = participants });
		Ui::AddExpandablePeerList(report, controller, inner);
		handleSubmition(report);

		const auto ids = items.front()->from()->owner().itemsToIds(items);
		handleConfirmation(report, controller, [=](
				not_null<PeerData*> p,
				not_null<ChannelData*> c) {
			auto filtered = ranges::views::all(
				ids
			) | ranges::views::transform([](const FullMsgId &id) {
				return MTP_int(id.msg);
			}) | ranges::to<QVector<MTPint>>();
			c->session().api().request(
				MTPchannels_ReportSpam(
					c->inputChannel,
					p->input,
					MTP_vector<MTPint>(std::move(filtered)))
			).send();
		});
	}

	if (allCanDelete) {
		Ui::AddSkip(inner);
		Ui::AddSkip(inner);

		const auto deleteAll = inner->add(
			object_ptr<Ui::Checkbox>(
				inner,
				!(isSingle)
					? tr::lng_delete_all_from_users(
						tr::now,
						Ui::Text::WithEntities)
					: tr::lng_delete_all_from_user(
						tr::now,
						lt_user,
						Ui::Text::Bold(items.front()->from()->name()),
						Ui::Text::WithEntities),
				false,
				st::defaultBoxCheckbox),
			st::boxRowPadding + buttonPadding);
		if (isSingle) {
			const auto history = items.front()->history();
			tr::lng_selected_delete_sure(
				lt_count,
				rpl::combine(
					MessagesCountValue(history, participants.front()),
					deleteAll->checkedValue()
				) | rpl::map([s = items.size()](int all, bool checked) {
					return float64((checked && all) ? all : s);
				})
			) | rpl::start_with_next([=](const QString &text) {
				title->setText(text);
				title->resizeToWidth(inner->width()
					- rect::m::sum::h(st::boxRowPadding));
			}, title->lifetime());
		}

		const auto controller = box->lifetime().make_state<Controller>(
			Controller::Data{ .participants = participants });
		Ui::AddExpandablePeerList(deleteAll, controller, inner);
		handleSubmition(deleteAll);

		handleConfirmation(deleteAll, controller, [=](
				not_null<PeerData*> p,
				not_null<ChannelData*> c) {
			p->session().api().deleteAllFromParticipant(c, p);
		});
	}
	if (allCanBan) {
		auto ownedWrap = object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			inner,
			object_ptr<Ui::VerticalLayout>(inner));

		Ui::AddSkip(inner);
		Ui::AddSkip(inner);
		const auto ban = inner->add(
			object_ptr<Ui::Checkbox>(
				box,
				rpl::conditional(
					ownedWrap->toggledValue(),
					tr::lng_context_restrict_user(),
					rpl::conditional(
						rpl::single(isSingle),
						tr::lng_ban_user(),
						tr::lng_ban_users())),
				false,
				st::defaultBoxCheckbox),
			st::boxRowPadding + buttonPadding);
		const auto controller = box->lifetime().make_state<Controller>(
			Controller::Data{ .participants = participants });
		Ui::AddExpandablePeerList(ban, controller, inner);
		handleSubmition(ban);

		Ui::AddSkip(inner);
		Ui::AddSkip(inner);

		const auto wrap = inner->add(std::move(ownedWrap));
		const auto container = wrap->entity();
		wrap->toggle(false, anim::type::instant);

		const auto session = &participants.front()->session();
		const auto emojiMargin = QMargins(
			-st::moderateBoxExpandInnerSkip,
			-st::moderateBoxExpandInnerSkip / 2,
			0,
			0);
		const auto emojiUp = Ui::Text::SingleCustomEmoji(
			session->data().customEmojiManager().registerInternalEmoji(
				st::moderateBoxExpandIcon,
				emojiMargin,
				false));
		const auto emojiDown = Ui::Text::SingleCustomEmoji(
			session->data().customEmojiManager().registerInternalEmoji(
				st::moderateBoxExpandIconDown,
				emojiMargin,
				false));

		auto label = object_ptr<Ui::FlatLabel>(
			inner,
			QString(),
			st::moderateBoxDividerLabel);
		const auto raw = label.data();

		auto &lifetime = wrap->lifetime();
		const auto scrollLifetime = lifetime.make_state<rpl::lifetime>();
		label->setClickHandlerFilter([=](
				const ClickHandlerPtr &handler,
				Qt::MouseButton button) {
			if (button != Qt::LeftButton) {
				return false;
			}
			wrap->toggle(!wrap->toggled(), anim::type::normal);
			{
				inner->heightValue() | rpl::start_with_next([=] {
					if (!wrap->animating()) {
						scrollLifetime->destroy();
						Ui::PostponeCall(crl::guard(box, [=] {
							box->scrollToY(std::numeric_limits<int>::max());
						}));
					} else {
						box->scrollToY(std::numeric_limits<int>::max());
					}
				}, *scrollLifetime);
			}
			return true;
		});
		wrap->toggledValue(
		) | rpl::map([isSingle, emojiUp, emojiDown](bool toggled) {
			return ((toggled && isSingle)
				? tr::lng_restrict_user_part
				: (toggled && !isSingle)
				? tr::lng_restrict_users_part
				: isSingle
				? tr::lng_restrict_user_full
				: tr::lng_restrict_users_full)(
					lt_emoji,
					rpl::single(toggled ? emojiUp : emojiDown),
					Ui::Text::WithEntities);
		}) | rpl::flatten_latest(
		) | rpl::start_with_next([=](const TextWithEntities &text) {
			raw->setMarkedText(
				Ui::Text::Link(text, u"internal:"_q),
				Core::MarkedTextContext{
					.session = session,
					.customEmojiRepaint = [=] { raw->update(); },
				});
		}, label->lifetime());

		Ui::AddSkip(inner);
		inner->add(object_ptr<Ui::DividerLabel>(
			inner,
			std::move(label),
			st::defaultBoxDividerLabelPadding,
			RectPart::Top | RectPart::Bottom));

		using Flag = ChatRestriction;
		using Flags = ChatRestrictions;
		const auto peer = items.front()->history()->peer;
		const auto chat = peer->asChat();
		const auto channel = peer->asChannel();
		const auto defaultRestrictions = chat
			? chat->defaultRestrictions()
			: channel->defaultRestrictions();
		const auto prepareFlags = FixDependentRestrictions(
			defaultRestrictions
			| ((channel && channel->isPublic())
				? (Flag::ChangeInfo | Flag::PinMessages)
				: Flags(0)));
		const auto disabledMessages = [&] {
			auto result = base::flat_map<Flags, QString>();
			{
				const auto disabled = FixDependentRestrictions(
					defaultRestrictions
					| ((channel && channel->isPublic())
						? (Flag::ChangeInfo | Flag::PinMessages)
						: Flags(0)));
				result.emplace(
					disabled,
					tr::lng_rights_restriction_for_all(tr::now));
			}
			return result;
		}();

		auto [checkboxes, getRestrictions, changes] = CreateEditRestrictions(
			box,
			rpl::conditional(
				rpl::single(isSingle),
				tr::lng_restrict_users_part_single_header(),
				tr::lng_restrict_users_part_header(
					lt_count,
					rpl::single(participants.size()) | tr::to_count())),
			prepareFlags,
			disabledMessages,
			{ .isForum = peer->isForum() });
		std::move(changes) | rpl::start_with_next([=] {
			ban->setChecked(true);
		}, ban->lifetime());
		Ui::AddSkip(container);
		Ui::AddDivider(container);
		Ui::AddSkip(container);
		container->add(std::move(checkboxes));

		// Handle confirmation manually.
		confirms->events() | rpl::start_with_next([=] {
			if (ban->checked() && controller->collectRequests) {
				const auto kick = !wrap->toggled();
				const auto restrictions = getRestrictions();
				const auto request = [=](
						not_null<PeerData*> peer,
						not_null<ChannelData*> channel) {
					if (!kick) {
						Api::ChatParticipants::Restrict(
							channel,
							peer,
							ChatRestrictionsInfo(), // Unused.
							ChatRestrictionsInfo(restrictions, 0),
							nullptr,
							nullptr);
					} else {
						channel->session().api().chatParticipants().kick(
							channel,
							peer,
							{ channel->restrictions(), 0 });
					}
				};
				sequentiallyRequest(request, controller->collectRequests());
			}
		}, ban->lifetime());
	}

	const auto close = crl::guard(box, [=] { box->closeBox(); });
	{
		const auto data = &participants.front()->session().data();
		const auto ids = data->itemsToIds(items);
		box->addButton(tr::lng_box_delete(), [=] {
			confirms->fire({});
			if (confirmed) {
				confirmed();
			}
			data->histories().deleteMessages(ids, true);
			data->sendHistoryChangeNotifications();
			close();
		});
	}
	box->addButton(tr::lng_cancel(), close);
}

bool CanCreateModerateMessagesBox(const HistoryItemsList &items) {
	const auto options = CalculateModerateOptions(items);
	return (options.allCanBan || options.allCanDelete)
		&& !options.participants.empty();
}

void DeleteChatBox(not_null<Ui::GenericBox*> box, not_null<PeerData*> peer) {
	const auto container = box->verticalLayout();

	const auto maybeUser = peer->asUser();

	Ui::AddSkip(container);
	Ui::AddSkip(container);

	base::install_event_filter(box, [=](not_null<QEvent*> event) {
		if (event->type() == QEvent::KeyPress) {
			if (const auto k = static_cast<QKeyEvent*>(event.get())) {
				if ((k->key() == Qt::Key_Enter)
					|| (k->key() == Qt::Key_Return)) {
					box->uiShow()->show(Ui::MakeConfirmBox({
						.text = tr::lng_gigagroup_warning_title(),
						.confirmed = [=](Fn<void()> close) {
							box->triggerButton(0);
							close();
						},
						.confirmText = tr::lng_box_yes(),
						.cancelText = tr::lng_box_no(),
					}));
				}
			}
		}
		return base::EventFilterResult::Continue;
	});

	const auto userpic = Ui::CreateChild<Ui::UserpicButton>(
		container,
		peer,
		st::mainMenuUserpic);
	userpic->showSavedMessagesOnSelf(true);
	Ui::IconWithTitle(
		container,
		userpic,
		Ui::CreateChild<Ui::FlatLabel>(
			container,
			peer->isSelf()
				? tr::lng_saved_messages() | Ui::Text::ToBold()
				: maybeUser
				? tr::lng_profile_delete_conversation() | Ui::Text::ToBold()
				: rpl::single(
					peer->name()
				) | Ui::Text::ToBold() | rpl::type_erased(),
			box->getDelegate()->style().title));

	Ui::AddSkip(container);
	Ui::AddSkip(container);

	box->addRow(
		object_ptr<Ui::FlatLabel>(
			container,
			peer->isSelf()
				? tr::lng_sure_delete_saved_messages()
				: maybeUser
				? tr::lng_sure_delete_history(
					lt_contact,
					rpl::single(peer->name()))
				: (peer->isChannel() && !peer->isMegagroup())
				? tr::lng_sure_leave_channel()
				: tr::lng_sure_leave_group(),
			st::boxLabel));

	const auto maybeCheckbox = [&]() -> Ui::Checkbox* {
		if (!peer->canRevokeFullHistory()) {
			return nullptr;
		}
		Ui::AddSkip(container);
		Ui::AddSkip(container);
		return box->addRow(
			object_ptr<Ui::Checkbox>(
				container,
				maybeUser
					? tr::lng_delete_for_other_check(
						tr::now,
						lt_user,
						TextWithEntities{ maybeUser->firstName },
						Ui::Text::RichLangValue)
					: tr::lng_delete_for_everyone_check(
						tr::now,
						Ui::Text::WithEntities),
				false,
				st::defaultBoxCheckbox));
	}();

	const auto maybeBotCheckbox = [&]() -> Ui::Checkbox* {
		if (!maybeUser || !maybeUser->isBot()) {
			return nullptr;
		}
		Ui::AddSkip(container);
		Ui::AddSkip(container);
		return box->addRow(
			object_ptr<Ui::Checkbox>(
				container,
				tr::lng_profile_block_bot(tr::now, Ui::Text::WithEntities),
				false,
				st::defaultBoxCheckbox));
	}();

	Ui::AddSkip(container);

	auto buttonText = maybeUser
		? tr::lng_box_delete()
		: !maybeCheckbox
		? tr::lng_box_leave()
		: maybeCheckbox->checkedValue() | rpl::map([](bool checked) {
			return checked ? tr::lng_box_delete() : tr::lng_box_leave();
		}) | rpl::flatten_latest();

	const auto close = crl::guard(box, [=] { box->closeBox(); });
	box->addButton(std::move(buttonText), [=] {
		const auto revoke = maybeCheckbox && maybeCheckbox->checked();
		const auto stopBot = maybeBotCheckbox && maybeBotCheckbox->checked();
		Core::App().closeChatFromWindows(peer);
		if (stopBot) {
			peer->session().api().blockedPeers().block(peer);
		}
		// Don't delete old history by default,
		// because Android app doesn't.
		//
		//if (const auto from = peer->migrateFrom()) {
		//	peer->session().api().deleteConversation(from, false);
		//}
		peer->session().api().deleteConversation(peer, revoke);
		close();
	}, st::attentionBoxButton);
	box->addButton(tr::lng_cancel(), close);
}
