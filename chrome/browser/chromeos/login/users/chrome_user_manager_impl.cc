// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/login/users/chrome_user_manager_impl.h"

#include <stddef.h>

#include <cstddef>
#include <set>
#include <utility>

#include "ash/common/multi_profile_uma.h"
#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/format_macros.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/metrics/histogram.h"
#include "base/single_thread_task_runner.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/sys_info.h"
#include "base/task_runner.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/values.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/chrome_notification_types.h"
#include "chrome/browser/chromeos/login/demo_mode/demo_app_launcher.h"
#include "chrome/browser/chromeos/login/session/user_session_manager.h"
#include "chrome/browser/chromeos/login/signin/auth_sync_observer.h"
#include "chrome/browser/chromeos/login/signin/auth_sync_observer_factory.h"
#include "chrome/browser/chromeos/login/users/affiliation.h"
#include "chrome/browser/chromeos/login/users/avatar/user_image_manager_impl.h"
#include "chrome/browser/chromeos/login/users/chrome_user_manager_util.h"
#include "chrome/browser/chromeos/login/users/default_user_image/default_user_images.h"
#include "chrome/browser/chromeos/login/users/multi_profile_user_controller.h"
#include "chrome/browser/chromeos/login/users/supervised_user_manager_impl.h"
#include "chrome/browser/chromeos/login/users/wallpaper/wallpaper_manager.h"
#include "chrome/browser/chromeos/policy/browser_policy_connector_chromeos.h"
#include "chrome/browser/chromeos/profiles/multiprofiles_session_aborted_dialog.h"
#include "chrome/browser/chromeos/profiles/profile_helper.h"
#include "chrome/browser/chromeos/session_length_limiter.h"
#include "chrome/browser/chromeos/settings/cros_settings.h"
#include "chrome/browser/chromeos/system/timezone_resolver_manager.h"
#include "chrome/browser/chromeos/system/timezone_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/signin/easy_unlock_service.h"
#include "chrome/browser/supervised_user/chromeos/manager_password_service_factory.h"
#include "chrome/browser/supervised_user/chromeos/supervised_user_password_service_factory.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/crash_keys.h"
#include "chrome/common/pref_names.h"
#include "chrome/grit/theme_resources.h"
#include "chromeos/chromeos_switches.h"
#include "chromeos/cryptohome/async_method_caller.h"
#include "chromeos/login/login_state.h"
#include "chromeos/login/user_names.h"
#include "chromeos/settings/cros_settings_names.h"
#include "chromeos/timezone/timezone_resolver.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "components/session_manager/core/session_manager.h"
#include "components/signin/core/account_id/account_id.h"
#include "components/user_manager/known_user.h"
#include "components/user_manager/remove_user_delegate.h"
#include "components/user_manager/user_image/user_image.h"
#include "components/user_manager/user_type.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/notification_service.h"
#include "policy/policy_constants.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/chromeos/resources/grit/ui_chromeos_resources.h"
#include "ui/chromeos/strings/grit/ui_chromeos_strings.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/wm/core/wm_core_switches.h"

using content::BrowserThread;

namespace chromeos {
namespace {

// A vector pref of the the regular users known on this device, arranged in LRU
// order.
const char kRegularUsers[] = "LoggedInUsers";

// A vector pref of the device local accounts defined on this device.
const char kDeviceLocalAccounts[] = "PublicAccounts";

// Key for list of users that should be reported.
const char kReportingUsers[] = "reporting_users";

// A string pref that gets set when a device local account is removed but a
// user is currently logged into that account, requiring the account's data to
// be removed after logout.
const char kDeviceLocalAccountPendingDataRemoval[] =
    "PublicAccountPendingDataRemoval";

bool FakeOwnership() {
  return base::CommandLine::ForCurrentProcess()->HasSwitch(
      switches::kStubCrosSettings);
}

std::string FullyCanonicalize(const std::string& email) {
  return gaia::CanonicalizeEmail(gaia::SanitizeEmail(email));
}

// Callback that is called after user removal is complete.
void OnRemoveUserComplete(const AccountId& account_id,
                          bool success,
                          cryptohome::MountError return_code) {
  // Log the error, but there's not much we can do.
  if (!success) {
    LOG(ERROR) << "Removal of cryptohome for " << account_id.Serialize()
               << " failed, return code: " << return_code;
  }
}

// Runs on SequencedWorkerPool thread. Passes resolved locale to UI thread.
void ResolveLocale(const std::string& raw_locale,
                   std::string* resolved_locale) {
  ignore_result(l10n_util::CheckAndResolveLocale(raw_locale, resolved_locale));
}

bool GetUserLockAttributes(const user_manager::User* user,
                           bool* can_lock,
                           std::string* multi_profile_behavior) {
  Profile* const profile = ProfileHelper::Get()->GetProfileByUser(user);
  if (!profile)
    return false;
  PrefService* const prefs = profile->GetPrefs();
  if (can_lock)
    *can_lock = user->can_lock() && prefs->GetBoolean(prefs::kAllowScreenLock);
  if (multi_profile_behavior) {
    *multi_profile_behavior =
        prefs->GetString(prefs::kMultiProfileUserBehavior);
  }
  return true;
}

}  // namespace

// static
void ChromeUserManagerImpl::RegisterPrefs(PrefRegistrySimple* registry) {
  ChromeUserManager::RegisterPrefs(registry);

  registry->RegisterListPref(kDeviceLocalAccounts);
  registry->RegisterStringPref(kDeviceLocalAccountPendingDataRemoval,
                               std::string());
  registry->RegisterListPref(kReportingUsers);

  SupervisedUserManager::RegisterPrefs(registry);
  SessionLengthLimiter::RegisterPrefs(registry);
  BootstrapManager::RegisterPrefs(registry);
}

// static
std::unique_ptr<ChromeUserManager>
ChromeUserManagerImpl::CreateChromeUserManager() {
  return std::unique_ptr<ChromeUserManager>(new ChromeUserManagerImpl());
}

ChromeUserManagerImpl::ChromeUserManagerImpl()
    : ChromeUserManager(base::ThreadTaskRunnerHandle::Get()),
      cros_settings_(CrosSettings::Get()),
      device_local_account_policy_service_(NULL),
      supervised_user_manager_(new SupervisedUserManagerImpl(this)),
      bootstrap_manager_(new BootstrapManager(this)),
      weak_factory_(this) {
  UpdateNumberOfUsers();

  // UserManager instance should be used only on UI thread.
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  registrar_.Add(this,
                 chrome::NOTIFICATION_OWNERSHIP_STATUS_CHANGED,
                 content::NotificationService::AllSources());
  registrar_.Add(this,
                 chrome::NOTIFICATION_LOGIN_USER_PROFILE_PREPARED,
                 content::NotificationService::AllSources());
  registrar_.Add(this,
                 chrome::NOTIFICATION_PROFILE_CREATED,
                 content::NotificationService::AllSources());

  // Since we're in ctor postpone any actions till this is fully created.
  if (base::ThreadTaskRunnerHandle::IsSet()) {
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE,
        base::Bind(&ChromeUserManagerImpl::RetrieveTrustedDevicePolicies,
                   weak_factory_.GetWeakPtr()));
  }

  local_accounts_subscription_ = cros_settings_->AddSettingsObserver(
      kAccountsPrefDeviceLocalAccounts,
      base::Bind(&ChromeUserManagerImpl::RetrieveTrustedDevicePolicies,
                 weak_factory_.GetWeakPtr()));
  multi_profile_user_controller_.reset(
      new MultiProfileUserController(this, GetLocalState()));

  policy::BrowserPolicyConnectorChromeOS* connector =
      g_browser_process->platform_part()->browser_policy_connector_chromeos();
  avatar_policy_observer_.reset(new policy::CloudExternalDataPolicyObserver(
      cros_settings_,
      connector->GetDeviceLocalAccountPolicyService(),
      policy::key::kUserAvatarImage,
      this));
  avatar_policy_observer_->Init();

  wallpaper_policy_observer_.reset(new policy::CloudExternalDataPolicyObserver(
      cros_settings_,
      connector->GetDeviceLocalAccountPolicyService(),
      policy::key::kWallpaperImage,
      this));
  wallpaper_policy_observer_->Init();
}

ChromeUserManagerImpl::~ChromeUserManagerImpl() {
}

void ChromeUserManagerImpl::Shutdown() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  ChromeUserManager::Shutdown();

  local_accounts_subscription_.reset();

  // Stop the session length limiter.
  session_length_limiter_.reset();

  if (device_local_account_policy_service_)
    device_local_account_policy_service_->RemoveObserver(this);

  for (UserImageManagerMap::iterator it = user_image_managers_.begin(),
                                     ie = user_image_managers_.end();
       it != ie;
       ++it) {
    it->second->Shutdown();
  }
  multi_profile_user_controller_.reset();
  avatar_policy_observer_.reset();
  wallpaper_policy_observer_.reset();
  registrar_.RemoveAll();
}

BootstrapManager* ChromeUserManagerImpl::GetBootstrapManager() {
  return bootstrap_manager_.get();
}

MultiProfileUserController*
ChromeUserManagerImpl::GetMultiProfileUserController() {
  return multi_profile_user_controller_.get();
}

UserImageManager* ChromeUserManagerImpl::GetUserImageManager(
    const AccountId& account_id) {
  UserImageManagerMap::iterator ui = user_image_managers_.find(account_id);
  if (ui != user_image_managers_.end())
    return ui->second.get();
  linked_ptr<UserImageManagerImpl> mgr(
      new UserImageManagerImpl(account_id.GetUserEmail(), this));
  user_image_managers_[account_id] = mgr;
  return mgr.get();
}

SupervisedUserManager* ChromeUserManagerImpl::GetSupervisedUserManager() {
  return supervised_user_manager_.get();
}

user_manager::UserList ChromeUserManagerImpl::GetUsersAllowedForMultiProfile()
    const {
  // Supervised users are not allowed to use multi-profiles.
  if (GetLoggedInUsers().size() == 1 &&
      GetPrimaryUser()->GetType() != user_manager::USER_TYPE_REGULAR) {
    return user_manager::UserList();
  }

  user_manager::UserList result;
  const user_manager::UserList& users = GetUsers();
  for (user_manager::UserList::const_iterator it = users.begin();
       it != users.end();
       ++it) {
    if ((*it)->GetType() == user_manager::USER_TYPE_REGULAR &&
        !(*it)->is_logged_in()) {
      MultiProfileUserController::UserAllowedInSessionReason check;
      multi_profile_user_controller_->IsUserAllowedInSession((*it)->email(),
                                                             &check);
      if (check ==
          MultiProfileUserController::NOT_ALLOWED_PRIMARY_USER_POLICY_FORBIDS) {
        return user_manager::UserList();
      }

      // Users with a policy that prevents them being added to a session will be
      // shown in login UI but will be grayed out.
      // Same applies to owner account (see http://crbug.com/385034).
      result.push_back(*it);
    }
  }

  return result;
}

user_manager::UserList
ChromeUserManagerImpl::GetUsersAllowedForSupervisedUsersCreation() const {
  CrosSettings* cros_settings = CrosSettings::Get();
  bool allow_new_user = true;
  cros_settings->GetBoolean(kAccountsPrefAllowNewUser, &allow_new_user);
  bool supervised_users_allowed = AreSupervisedUsersAllowed();

  // Restricted either by policy or by owner.
  if (!allow_new_user || !supervised_users_allowed)
    return user_manager::UserList();

  return GetUsersAllowedAsSupervisedUserManagers(GetUsers());
}

user_manager::UserList ChromeUserManagerImpl::GetUnlockUsers() const {
  const user_manager::UserList& logged_in_users = GetLoggedInUsers();
  if (logged_in_users.empty())
    return user_manager::UserList();

  bool can_primary_lock = false;
  std::string primary_behavior;
  if (!GetUserLockAttributes(GetPrimaryUser(), &can_primary_lock,
                             &primary_behavior)) {
    // Locking is not allowed until the primary user profile is created.
    return user_manager::UserList();
  }

  user_manager::UserList unlock_users;

  // Specific case: only one logged in user or
  // primary user has primary-only multi-profile policy.
  if (logged_in_users.size() == 1 ||
      primary_behavior == MultiProfileUserController::kBehaviorPrimaryOnly) {
    if (can_primary_lock)
      unlock_users.push_back(primary_user_);
  } else {
    // Fill list of potential unlock users based on multi-profile policy state.
    for (user_manager::User* user : logged_in_users) {
      bool can_lock = false;
      std::string behavior;
      if (!GetUserLockAttributes(user, &can_lock, &behavior))
        continue;
      if (behavior == MultiProfileUserController::kBehaviorUnrestricted &&
          can_lock) {
        unlock_users.push_back(user);
      } else if (behavior == MultiProfileUserController::kBehaviorPrimaryOnly) {
        NOTREACHED()
            << "Spotted primary-only multi-profile policy for non-primary user";
      }
    }
  }

  return unlock_users;
}

void ChromeUserManagerImpl::SessionStarted() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  ChromeUserManager::SessionStarted();

  content::NotificationService::current()->Notify(
      chrome::NOTIFICATION_SESSION_STARTED,
      content::Source<UserManager>(this),
      content::Details<const user_manager::User>(GetActiveUser()));
}

void ChromeUserManagerImpl::RemoveUserInternal(
    const AccountId& account_id,
    user_manager::RemoveUserDelegate* delegate) {
  CrosSettings* cros_settings = CrosSettings::Get();

  const base::Closure& callback =
      base::Bind(&ChromeUserManagerImpl::RemoveUserInternal,
                 weak_factory_.GetWeakPtr(), account_id, delegate);

  // Ensure the value of owner email has been fetched.
  if (CrosSettingsProvider::TRUSTED !=
      cros_settings->PrepareTrustedValues(callback)) {
    // Value of owner email is not fetched yet.  RemoveUserInternal will be
    // called again after fetch completion.
    return;
  }
  std::string owner;
  cros_settings->GetString(kDeviceOwner, &owner);
  if (account_id == AccountId::FromUserEmail(owner)) {
    // Owner is not allowed to be removed from the device.
    return;
  }
  RemoveNonOwnerUserInternal(account_id, delegate);
}

void ChromeUserManagerImpl::SaveUserOAuthStatus(
    const AccountId& account_id,
    user_manager::User::OAuthTokenStatus oauth_token_status) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  ChromeUserManager::SaveUserOAuthStatus(account_id, oauth_token_status);

  GetUserFlow(account_id)->HandleOAuthTokenStatusChange(oauth_token_status);
}

void ChromeUserManagerImpl::SaveUserDisplayName(
    const AccountId& account_id,
    const base::string16& display_name) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  ChromeUserManager::SaveUserDisplayName(account_id, display_name);

  // Do not update local state if data stored or cached outside the user's
  // cryptohome is to be treated as ephemeral.
  if (!IsUserNonCryptohomeDataEphemeral(account_id)) {
    supervised_user_manager_->UpdateManagerName(account_id.GetUserEmail(),
                                                display_name);
  }
}

void ChromeUserManagerImpl::StopPolicyObserverForTesting() {
  avatar_policy_observer_.reset();
  wallpaper_policy_observer_.reset();
}

void ChromeUserManagerImpl::Observe(
    int type,
    const content::NotificationSource& source,
    const content::NotificationDetails& details) {
  switch (type) {
    case chrome::NOTIFICATION_OWNERSHIP_STATUS_CHANGED:
      if (!device_local_account_policy_service_) {
        policy::BrowserPolicyConnectorChromeOS* connector =
            g_browser_process->platform_part()
                ->browser_policy_connector_chromeos();
        device_local_account_policy_service_ =
            connector->GetDeviceLocalAccountPolicyService();
        if (device_local_account_policy_service_)
          device_local_account_policy_service_->AddObserver(this);
      }
      RetrieveTrustedDevicePolicies();
      UpdateOwnership();
      break;
    case chrome::NOTIFICATION_LOGIN_USER_PROFILE_PREPARED: {
      Profile* profile = content::Details<Profile>(details).ptr();
      if (IsUserLoggedIn() && !IsLoggedInAsGuest() && !IsLoggedInAsKioskApp()) {
        if (IsLoggedInAsSupervisedUser())
          SupervisedUserPasswordServiceFactory::GetForProfile(profile);
        if (IsLoggedInAsUserWithGaiaAccount())
          ManagerPasswordServiceFactory::GetForProfile(profile);

        if (!profile->IsOffTheRecord()) {
          AuthSyncObserver* sync_observer =
              AuthSyncObserverFactory::GetInstance()->GetForProfile(profile);
          sync_observer->StartObserving();
          multi_profile_user_controller_->StartObserving(profile);
        }
      }
      UpdateUserTimeZoneRefresher(profile);
      break;
    }
    case chrome::NOTIFICATION_PROFILE_CREATED: {
      Profile* profile = content::Source<Profile>(source).ptr();
      user_manager::User* user =
          ProfileHelper::Get()->GetUserByProfile(profile);
      if (user != NULL) {
        user->set_profile_is_created();

        if (user->HasGaiaAccount())
          GetUserImageManager(user->GetAccountId())->UserProfileCreated();
      }

      // If there is pending user switch, do it now.
      if (GetPendingUserSwitchID().is_valid()) {
        // Call SwitchActiveUser async because otherwise it may cause
        // ProfileManager::GetProfile before the profile gets registered
        // in ProfileManager. It happens in case of sync profile load when
        // NOTIFICATION_PROFILE_CREATED is called synchronously.
        base::ThreadTaskRunnerHandle::Get()->PostTask(
            FROM_HERE,
            base::Bind(&ChromeUserManagerImpl::SwitchActiveUser,
                       weak_factory_.GetWeakPtr(), GetPendingUserSwitchID()));
        SetPendingUserSwitchId(EmptyAccountId());
      }
      break;
    }
    default:
      NOTREACHED();
  }
}

void ChromeUserManagerImpl::OnExternalDataSet(const std::string& policy,
                                              const std::string& user_id) {
  const AccountId account_id =
      user_manager::known_user::GetAccountId(user_id, std::string());
  if (policy == policy::key::kUserAvatarImage)
    GetUserImageManager(account_id)->OnExternalDataSet(policy);
  else if (policy == policy::key::kWallpaperImage)
    WallpaperManager::Get()->OnPolicySet(policy, account_id);
  else
    NOTREACHED();
}

void ChromeUserManagerImpl::OnExternalDataCleared(const std::string& policy,
                                                  const std::string& user_id) {
  const AccountId account_id =
      user_manager::known_user::GetAccountId(user_id, std::string());
  if (policy == policy::key::kUserAvatarImage)
    GetUserImageManager(account_id)->OnExternalDataCleared(policy);
  else if (policy == policy::key::kWallpaperImage)
    WallpaperManager::Get()->OnPolicyCleared(policy, account_id);
  else
    NOTREACHED();
}

void ChromeUserManagerImpl::OnExternalDataFetched(
    const std::string& policy,
    const std::string& user_id,
    std::unique_ptr<std::string> data) {
  const AccountId account_id =
      user_manager::known_user::GetAccountId(user_id, std::string());
  if (policy == policy::key::kUserAvatarImage)
    GetUserImageManager(account_id)
        ->OnExternalDataFetched(policy, std::move(data));
  else if (policy == policy::key::kWallpaperImage)
    WallpaperManager::Get()->OnPolicyFetched(policy, account_id,
                                             std::move(data));
  else
    NOTREACHED();
}

void ChromeUserManagerImpl::OnPolicyUpdated(const std::string& user_id) {
  const AccountId account_id =
      user_manager::known_user::GetAccountId(user_id, std::string());
  const user_manager::User* user = FindUser(account_id);
  if (!user || user->GetType() != user_manager::USER_TYPE_PUBLIC_ACCOUNT)
    return;
  UpdatePublicAccountDisplayName(user_id);
}

void ChromeUserManagerImpl::OnDeviceLocalAccountsChanged() {
  // No action needed here, changes to the list of device-local accounts get
  // handled via the kAccountsPrefDeviceLocalAccounts device setting observer.
}

bool ChromeUserManagerImpl::CanCurrentUserLock() const {
  if (!ChromeUserManager::CanCurrentUserLock() ||
      !GetCurrentUserFlow()->CanLockScreen()) {
    return false;
  }
  bool can_lock = false;
  if (!GetUserLockAttributes(active_user_, &can_lock, nullptr))
    return false;
  return can_lock;
}

bool ChromeUserManagerImpl::IsUserNonCryptohomeDataEphemeral(
    const AccountId& account_id) const {
  // Data belonging to the obsolete device local accounts whose data has not
  // been removed yet is not ephemeral.
  const bool is_obsolete_device_local_account =
      IsDeviceLocalAccountMarkedForRemoval(account_id);

  return !is_obsolete_device_local_account &&
         ChromeUserManager::IsUserNonCryptohomeDataEphemeral(account_id);
}

bool ChromeUserManagerImpl::AreEphemeralUsersEnabled() const {
  policy::BrowserPolicyConnectorChromeOS* connector =
      g_browser_process->platform_part()->browser_policy_connector_chromeos();
  return GetEphemeralUsersEnabled() &&
         (connector->IsEnterpriseManaged() || GetOwnerAccountId().is_valid());
}

void ChromeUserManagerImpl::OnUserRemoved(const AccountId& account_id) {
  RemoveReportingUser(account_id);
}

const std::string& ChromeUserManagerImpl::GetApplicationLocale() const {
  return g_browser_process->GetApplicationLocale();
}

PrefService* ChromeUserManagerImpl::GetLocalState() const {
  return g_browser_process ? g_browser_process->local_state() : NULL;
}

void ChromeUserManagerImpl::HandleUserOAuthTokenStatusChange(
    const AccountId& account_id,
    user_manager::User::OAuthTokenStatus status) const {
  GetUserFlow(account_id)->HandleOAuthTokenStatusChange(status);
}

bool ChromeUserManagerImpl::IsEnterpriseManaged() const {
  policy::BrowserPolicyConnectorChromeOS* connector =
      g_browser_process->platform_part()->browser_policy_connector_chromeos();
  return connector->IsEnterpriseManaged();
}

void ChromeUserManagerImpl::LoadDeviceLocalAccounts(
    std::set<AccountId>* device_local_accounts_set) {
  const base::ListValue* prefs_device_local_accounts =
      GetLocalState()->GetList(kDeviceLocalAccounts);
  std::vector<AccountId> device_local_accounts;
  ParseUserList(*prefs_device_local_accounts, std::set<AccountId>(),
                &device_local_accounts, device_local_accounts_set);
  for (const AccountId& account_id : device_local_accounts) {
    policy::DeviceLocalAccount::Type type;
    if (!policy::IsDeviceLocalAccountUser(account_id.GetUserEmail(), &type)) {
      NOTREACHED();
      continue;
    }

    users_.push_back(
        CreateUserFromDeviceLocalAccount(account_id, type).release());
    if (type == policy::DeviceLocalAccount::TYPE_PUBLIC_SESSION)
      UpdatePublicAccountDisplayName(account_id.GetUserEmail());
  }
}

void ChromeUserManagerImpl::PerformPreUserListLoadingActions() {
  // Clean up user list first. All code down the path should be synchronous,
  // so that local state after transaction rollback is in consistent state.
  // This process also should not trigger EnsureUsersLoaded again.
  if (supervised_user_manager_->HasFailedUserCreationTransaction())
    supervised_user_manager_->RollbackUserCreationTransaction();

  // Abandon all unfinished bootstraps.
  bootstrap_manager_->RemoveAllPendingBootstrap();
}

void ChromeUserManagerImpl::PerformPostUserListLoadingActions() {
  for (user_manager::UserList::iterator ui = users_.begin(), ue = users_.end();
       ui != ue;
       ++ui) {
    GetUserImageManager((*ui)->GetAccountId())->LoadUserImage();
  }
}

void ChromeUserManagerImpl::PerformPostUserLoggedInActions(
    bool browser_restart) {
  // Initialize the session length limiter and start it only if
  // session limit is defined by the policy.
  session_length_limiter_.reset(
      new SessionLengthLimiter(NULL, browser_restart));
}

bool ChromeUserManagerImpl::IsDemoApp(const AccountId& account_id) const {
  return DemoAppLauncher::IsDemoAppSession(account_id);
}

bool ChromeUserManagerImpl::IsDeviceLocalAccountMarkedForRemoval(
    const AccountId& account_id) const {
  return account_id == AccountId::FromUserEmail(GetLocalState()->GetString(
                           kDeviceLocalAccountPendingDataRemoval));
}

void ChromeUserManagerImpl::RetrieveTrustedDevicePolicies() {
  // Local state may not be initialized in unit_tests.
  if (!GetLocalState())
    return;

  SetEphemeralUsersEnabled(false);
  SetOwnerId(EmptyAccountId());

  // Schedule a callback if device policy has not yet been verified.
  if (CrosSettingsProvider::TRUSTED !=
      cros_settings_->PrepareTrustedValues(
          base::Bind(&ChromeUserManagerImpl::RetrieveTrustedDevicePolicies,
                     weak_factory_.GetWeakPtr()))) {
    return;
  }

  bool ephemeral_users_enabled = false;
  cros_settings_->GetBoolean(kAccountsPrefEphemeralUsersEnabled,
                             &ephemeral_users_enabled);
  SetEphemeralUsersEnabled(ephemeral_users_enabled);

  std::string owner_email;
  cros_settings_->GetString(kDeviceOwner, &owner_email);
  SetOwnerId(AccountId::FromUserEmail(owner_email));

  EnsureUsersLoaded();

  bool changed = UpdateAndCleanUpDeviceLocalAccounts(
      policy::GetDeviceLocalAccounts(cros_settings_));

  // If ephemeral users are enabled and we are on the login screen, take this
  // opportunity to clean up by removing all regular users except the owner.
  if (GetEphemeralUsersEnabled() && !IsUserLoggedIn()) {
    ListPrefUpdate prefs_users_update(GetLocalState(), kRegularUsers);
    prefs_users_update->Clear();
    for (user_manager::UserList::iterator it = users_.begin();
         it != users_.end();) {
      const AccountId account_id = (*it)->GetAccountId();
      if ((*it)->HasGaiaAccount() && account_id != GetOwnerAccountId()) {
        RemoveNonCryptohomeData(account_id);
        DeleteUser(*it);
        it = users_.erase(it);
        changed = true;
      } else {
        if ((*it)->GetType() != user_manager::USER_TYPE_PUBLIC_ACCOUNT)
          prefs_users_update->Append(
              new base::StringValue(account_id.GetUserEmail()));
        ++it;
      }
    }
  }

  if (changed)
    NotifyUserListChanged();
}

void ChromeUserManagerImpl::GuestUserLoggedIn() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  ChromeUserManager::GuestUserLoggedIn();

  // TODO(nkostylev): Add support for passing guest session cryptohome
  // mount point. Legacy (--login-profile) value will be used for now.
  // http://crosbug.com/230859
  active_user_->SetStubImage(
      base::WrapUnique(new user_manager::UserImage(
          *ResourceBundle::GetSharedInstance().GetImageSkiaNamed(
              IDR_PROFILE_PICTURE_LOADING))),
      user_manager::User::USER_IMAGE_INVALID, false);

  // Initializes wallpaper after active_user_ is set.
  WallpaperManager::Get()->SetUserWallpaperNow(login::GuestAccountId());
}

void ChromeUserManagerImpl::RegularUserLoggedIn(const AccountId& account_id) {
  ChromeUserManager::RegularUserLoggedIn(account_id);

  if (FakeOwnership()) {
    const AccountId owner_account_id = GetActiveUser()->GetAccountId();
    VLOG(1) << "Set device owner to: " << owner_account_id.GetUserEmail();
    CrosSettings::Get()->SetString(kDeviceOwner,
                                   owner_account_id.GetUserEmail());
    SetOwnerId(owner_account_id);
  }

  if (IsCurrentUserNew())
    WallpaperManager::Get()->SetUserWallpaperNow(account_id);

  GetUserImageManager(account_id)->UserLoggedIn(IsCurrentUserNew(), false);

  WallpaperManager::Get()->EnsureLoggedInUserWallpaperLoaded();

  // Make sure that new data is persisted to Local State.
  GetLocalState()->CommitPendingWrite();
}

void ChromeUserManagerImpl::RegularUserLoggedInAsEphemeral(
    const AccountId& account_id) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  ChromeUserManager::RegularUserLoggedInAsEphemeral(account_id);

  GetUserImageManager(account_id)->UserLoggedIn(IsCurrentUserNew(), false);
  WallpaperManager::Get()->SetUserWallpaperNow(account_id);
}

void ChromeUserManagerImpl::SupervisedUserLoggedIn(
    const AccountId& account_id) {
  // TODO(nkostylev): Refactor, share code with RegularUserLoggedIn().

  // Remove the user from the user list.
  active_user_ = RemoveRegularOrSupervisedUserFromList(account_id);

  // If the user was not found on the user list, create a new user.
  if (!GetActiveUser()) {
    SetIsCurrentUserNew(true);
    active_user_ = user_manager::User::CreateSupervisedUser(account_id);
    // Leaving OAuth token status at the default state = unknown.
    WallpaperManager::Get()->SetUserWallpaperNow(account_id);
  } else {
    if (supervised_user_manager_->CheckForFirstRun(account_id.GetUserEmail())) {
      SetIsCurrentUserNew(true);
      WallpaperManager::Get()->SetUserWallpaperNow(account_id);
    } else {
      SetIsCurrentUserNew(false);
    }
  }

  // Add the user to the front of the user list.
  ListPrefUpdate prefs_users_update(GetLocalState(), kRegularUsers);
  prefs_users_update->Insert(0,
                             new base::StringValue(account_id.GetUserEmail()));
  users_.insert(users_.begin(), active_user_);

  // Now that user is in the list, save display name.
  if (IsCurrentUserNew()) {
    SaveUserDisplayName(GetActiveUser()->GetAccountId(),
                        GetActiveUser()->GetDisplayName());
  }

  GetUserImageManager(account_id)->UserLoggedIn(IsCurrentUserNew(), true);
  WallpaperManager::Get()->EnsureLoggedInUserWallpaperLoaded();

  // Make sure that new data is persisted to Local State.
  GetLocalState()->CommitPendingWrite();
}

bool ChromeUserManagerImpl::HasPendingBootstrap(
    const AccountId& account_id) const {
  return bootstrap_manager_->HasPendingBootstrap(account_id);
}

void ChromeUserManagerImpl::PublicAccountUserLoggedIn(
    user_manager::User* user) {
  SetIsCurrentUserNew(true);
  active_user_ = user;

  // The UserImageManager chooses a random avatar picture when a user logs in
  // for the first time. Tell the UserImageManager that this user is not new to
  // prevent the avatar from getting changed.
  GetUserImageManager(user->GetAccountId())->UserLoggedIn(false, true);
  WallpaperManager::Get()->EnsureLoggedInUserWallpaperLoaded();
}

void ChromeUserManagerImpl::KioskAppLoggedIn(user_manager::User* user) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  active_user_ = user;
  active_user_->SetStubImage(
      base::WrapUnique(new user_manager::UserImage(
          *ResourceBundle::GetSharedInstance().GetImageSkiaNamed(
              IDR_PROFILE_PICTURE_LOADING))),
      user_manager::User::USER_IMAGE_INVALID, false);

  const AccountId& kiosk_app_account_id = user->GetAccountId();
  WallpaperManager::Get()->SetUserWallpaperNow(kiosk_app_account_id);

  // TODO(bartfab): Add KioskAppUsers to the users_ list and keep metadata like
  // the kiosk_app_id in these objects, removing the need to re-parse the
  // device-local account list here to extract the kiosk_app_id.
  const std::vector<policy::DeviceLocalAccount> device_local_accounts =
      policy::GetDeviceLocalAccounts(cros_settings_);
  const policy::DeviceLocalAccount* account = NULL;
  for (std::vector<policy::DeviceLocalAccount>::const_iterator it =
           device_local_accounts.begin();
       it != device_local_accounts.end();
       ++it) {
    if (it->user_id == kiosk_app_account_id.GetUserEmail()) {
      account = &*it;
      break;
    }
  }
  std::string kiosk_app_id;
  if (account) {
    kiosk_app_id = account->kiosk_app_id;
  } else {
    LOG(ERROR) << "Logged into nonexistent kiosk-app account: "
               << kiosk_app_account_id.GetUserEmail();
    NOTREACHED();
  }

  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  command_line->AppendSwitch(::switches::kForceAppMode);
  command_line->AppendSwitchASCII(::switches::kAppId, kiosk_app_id);

  // Disable window animation since kiosk app runs in a single full screen
  // window and window animation causes start-up janks.
  command_line->AppendSwitch(wm::switches::kWindowAnimationsDisabled);
}

void ChromeUserManagerImpl::DemoAccountLoggedIn() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  active_user_ = user_manager::User::CreateKioskAppUser(login::DemoAccountId());
  active_user_->SetStubImage(
      base::WrapUnique(new user_manager::UserImage(
          *ResourceBundle::GetSharedInstance().GetImageSkiaNamed(
              IDR_PROFILE_PICTURE_LOADING))),
      user_manager::User::USER_IMAGE_INVALID, false);
  WallpaperManager::Get()->SetUserWallpaperNow(login::DemoAccountId());

  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  command_line->AppendSwitch(::switches::kForceAppMode);
  command_line->AppendSwitchASCII(::switches::kAppId,
                                  DemoAppLauncher::kDemoAppId);

  // Disable window animation since the demo app runs in a single full screen
  // window and window animation causes start-up janks.
  base::CommandLine::ForCurrentProcess()->AppendSwitch(
      wm::switches::kWindowAnimationsDisabled);
}

void ChromeUserManagerImpl::NotifyOnLogin() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  UserSessionManager::OverrideHomedir();
  UpdateNumberOfUsers();

  ChromeUserManager::NotifyOnLogin();

  // TODO(nkostylev): Deprecate this notification in favor of
  // ActiveUserChanged() observer call.
  content::NotificationService::current()->Notify(
      chrome::NOTIFICATION_LOGIN_USER_CHANGED,
      content::Source<UserManager>(this),
      content::Details<const user_manager::User>(GetActiveUser()));

  UserSessionManager::GetInstance()->PerformPostUserLoggedInActions();
}

void ChromeUserManagerImpl::UpdateOwnership() {
  bool is_owner =
      FakeOwnership() || DeviceSettingsService::Get()->HasPrivateOwnerKey();
  VLOG(1) << "Current user " << (is_owner ? "is owner" : "is not owner");

  SetCurrentUserIsOwner(is_owner);
}

void ChromeUserManagerImpl::RemoveNonCryptohomeData(
    const AccountId& account_id) {
  ChromeUserManager::RemoveNonCryptohomeData(account_id);

  WallpaperManager::Get()->RemoveUserWallpaperInfo(account_id);
  GetUserImageManager(account_id)->DeleteUserImage();

  supervised_user_manager_->RemoveNonCryptohomeData(account_id.GetUserEmail());

  multi_profile_user_controller_->RemoveCachedValues(account_id.GetUserEmail());

  EasyUnlockService::ResetLocalStateForUser(account_id);
}

void ChromeUserManagerImpl::
    CleanUpDeviceLocalAccountNonCryptohomeDataPendingRemoval() {
  PrefService* local_state = GetLocalState();
  const std::string device_local_account_pending_data_removal =
      local_state->GetString(kDeviceLocalAccountPendingDataRemoval);
  if (device_local_account_pending_data_removal.empty() ||
      (IsUserLoggedIn() &&
       device_local_account_pending_data_removal == GetActiveUser()->email())) {
    return;
  }

  RemoveNonCryptohomeData(
      AccountId::FromUserEmail(device_local_account_pending_data_removal));
  local_state->ClearPref(kDeviceLocalAccountPendingDataRemoval);
}

void ChromeUserManagerImpl::CleanUpDeviceLocalAccountNonCryptohomeData(
    const std::vector<std::string>& old_device_local_accounts) {
  std::set<std::string> users;
  for (user_manager::UserList::const_iterator it = users_.begin();
       it != users_.end();
       ++it)
    users.insert((*it)->email());

  // If the user is logged into a device local account that has been removed
  // from the user list, mark the account's data as pending removal after
  // logout.
  const user_manager::User* const active_user = GetActiveUser();
  if (active_user && active_user->IsDeviceLocalAccount()) {
    const std::string active_user_id = active_user->email();
    if (users.find(active_user_id) == users.end()) {
      GetLocalState()->SetString(kDeviceLocalAccountPendingDataRemoval,
                                 active_user_id);
      users.insert(active_user_id);
    }
  }

  // Remove the data belonging to any other device local accounts that are no
  // longer found on the user list.
  for (std::vector<std::string>::const_iterator it =
           old_device_local_accounts.begin();
       it != old_device_local_accounts.end(); ++it) {
    if (users.find(*it) == users.end())
      RemoveNonCryptohomeData(AccountId::FromUserEmail(*it));
  }
}

bool ChromeUserManagerImpl::UpdateAndCleanUpDeviceLocalAccounts(
    const std::vector<policy::DeviceLocalAccount>& device_local_accounts) {
  // Try to remove any device local account data marked as pending removal.
  CleanUpDeviceLocalAccountNonCryptohomeDataPendingRemoval();

  // Get the current list of device local accounts.
  std::vector<std::string> old_accounts;
  for (const auto& user : users_) {
    if (user->IsDeviceLocalAccount())
      old_accounts.push_back(user->email());
  }

  // If the list of device local accounts has not changed, return.
  if (device_local_accounts.size() == old_accounts.size()) {
    bool changed = false;
    for (size_t i = 0; i < device_local_accounts.size(); ++i) {
      if (device_local_accounts[i].user_id != old_accounts[i]) {
        changed = true;
        break;
      }
    }
    if (!changed)
      return false;
  }

  // Persist the new list of device local accounts in a pref.
  ListPrefUpdate prefs_device_local_accounts_update(GetLocalState(),
                                                    kDeviceLocalAccounts);
  prefs_device_local_accounts_update->Clear();
  for (const auto& account : device_local_accounts)
    prefs_device_local_accounts_update->AppendString(account.user_id);

  // Remove the old device local accounts from the user list.
  for (user_manager::UserList::iterator it = users_.begin();
       it != users_.end();) {
    if ((*it)->IsDeviceLocalAccount()) {
      if (*it != GetLoggedInUser())
        DeleteUser(*it);
      it = users_.erase(it);
    } else {
      ++it;
    }
  }

  // Add the new device local accounts to the front of the user list.
  user_manager::User* const active_user = GetActiveUser();
  const bool is_device_local_account_session =
      active_user && active_user->IsDeviceLocalAccount();
  for (auto it = device_local_accounts.rbegin();
       it != device_local_accounts.rend(); ++it) {
    if (is_device_local_account_session &&
        AccountId::FromUserEmail(it->user_id) == active_user->GetAccountId()) {
      users_.insert(users_.begin(), active_user);
    } else {
      users_.insert(users_.begin(),
                    CreateUserFromDeviceLocalAccount(
                        AccountId::FromUserEmail(it->user_id), it->type)
                        .release());
    }
    if (it->type == policy::DeviceLocalAccount::TYPE_PUBLIC_SESSION) {
      UpdatePublicAccountDisplayName(it->user_id);
    }
  }

  for (user_manager::UserList::iterator
           ui = users_.begin(),
           ue = users_.begin() + device_local_accounts.size();
       ui != ue; ++ui) {
    GetUserImageManager((*ui)->GetAccountId())->LoadUserImage();
  }

  // Remove data belonging to device local accounts that are no longer found on
  // the user list.
  CleanUpDeviceLocalAccountNonCryptohomeData(old_accounts);

  return true;
}

void ChromeUserManagerImpl::UpdatePublicAccountDisplayName(
    const std::string& user_id) {
  std::string display_name;

  if (device_local_account_policy_service_) {
    policy::DeviceLocalAccountPolicyBroker* broker =
        device_local_account_policy_service_->GetBrokerForUser(user_id);
    if (broker)
      display_name = broker->GetDisplayName();
  }

  // Set or clear the display name.
  SaveUserDisplayName(AccountId::FromUserEmail(user_id),
                      base::UTF8ToUTF16(display_name));
}

UserFlow* ChromeUserManagerImpl::GetCurrentUserFlow() const {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (!IsUserLoggedIn())
    return GetDefaultUserFlow();
  return GetUserFlow(GetLoggedInUser()->GetAccountId());
}

UserFlow* ChromeUserManagerImpl::GetUserFlow(
    const AccountId& account_id) const {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  FlowMap::const_iterator it = specific_flows_.find(account_id);
  if (it != specific_flows_.end())
    return it->second;
  return GetDefaultUserFlow();
}

void ChromeUserManagerImpl::SetUserFlow(const AccountId& account_id,
                                        UserFlow* flow) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  ResetUserFlow(account_id);
  specific_flows_[account_id] = flow;
}

void ChromeUserManagerImpl::ResetUserFlow(const AccountId& account_id) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  FlowMap::iterator it = specific_flows_.find(account_id);
  if (it != specific_flows_.end()) {
    delete it->second;
    specific_flows_.erase(it);
  }
}

bool ChromeUserManagerImpl::AreSupervisedUsersAllowed() const {
  bool supervised_users_allowed = false;
  cros_settings_->GetBoolean(kAccountsPrefSupervisedUsersEnabled,
                             &supervised_users_allowed);
  return supervised_users_allowed;
}

UserFlow* ChromeUserManagerImpl::GetDefaultUserFlow() const {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (!default_flow_.get())
    default_flow_.reset(new DefaultUserFlow());
  return default_flow_.get();
}

void ChromeUserManagerImpl::NotifyUserListChanged() {
  content::NotificationService::current()->Notify(
      chrome::NOTIFICATION_USER_LIST_CHANGED,
      content::Source<UserManager>(this),
      content::NotificationService::NoDetails());
}

void ChromeUserManagerImpl::NotifyUserAddedToSession(
    const user_manager::User* added_user,
    bool user_switch_pending) {
  // Special case for user session restoration after browser crash.
  // We don't switch to each user session that has been restored as once all
  // session will be restored we'll switch to the session that has been used
  // before the crash.
  if (user_switch_pending &&
      !UserSessionManager::GetInstance()->UserSessionsRestoreInProgress()) {
    SetPendingUserSwitchId(added_user->GetAccountId());
  }

  UpdateNumberOfUsers();
  ChromeUserManager::NotifyUserAddedToSession(added_user, user_switch_pending);
}

void ChromeUserManagerImpl::OnUserNotAllowed(const std::string& user_email) {
  LOG(ERROR) << "Shutdown session because a user is not allowed to be in the "
                "current session";
  chromeos::ShowMultiprofilesSessionAbortedDialog(user_email);
}

void ChromeUserManagerImpl::RemovePendingBootstrapUser(
    const AccountId& account_id) {
  DCHECK(HasPendingBootstrap(account_id));
  RemoveNonOwnerUserInternal(account_id, nullptr);
}

void ChromeUserManagerImpl::UpdateNumberOfUsers() {
  size_t users = GetLoggedInUsers().size();
  if (users) {
    // Write the user number as UMA stat when a multi user session is possible.
    if ((users + GetUsersAllowedForMultiProfile().size()) > 1)
      ash::MultiProfileUMA::RecordUserCount(users);
  }

  base::debug::SetCrashKeyValue(
      crash_keys::kNumberOfUsers,
      base::StringPrintf("%" PRIuS, GetLoggedInUsers().size()));
}

void ChromeUserManagerImpl::UpdateUserTimeZoneRefresher(Profile* profile) {
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          chromeos::switches::kDisableTimeZoneTrackingOption)) {
    return;
  }

  const user_manager::User* user =
      ProfileHelper::Get()->GetUserByProfile(profile);
  if (user == NULL)
    return;

  // In Multi-Profile mode only primary user settings are in effect.
  if (user != user_manager::UserManager::Get()->GetPrimaryUser())
    return;

  if (!IsUserLoggedIn())
    return;

  // Timezone auto refresh is disabled for Guest, Supervized and OffTheRecord
  // users, but enabled for Kiosk mode.
  if (IsLoggedInAsGuest() || IsLoggedInAsSupervisedUser() ||
      profile->IsOffTheRecord()) {
    g_browser_process->platform_part()->GetTimezoneResolver()->Stop();
    return;
  }
  g_browser_process->platform_part()
      ->GetTimezoneResolverManager()
      ->UpdateTimezoneResolver();
}

void ChromeUserManagerImpl::SetUserAffiliation(
    const std::string& user_email,
    const AffiliationIDSet& user_affiliation_ids) {
  const AccountId& account_id =
      user_manager::known_user::GetAccountId(user_email, std::string());
  user_manager::User* user = FindUserAndModify(account_id);

  if (user) {
    policy::BrowserPolicyConnectorChromeOS const* const connector =
        g_browser_process->platform_part()->browser_policy_connector_chromeos();
    const bool is_affiliated = chromeos::IsUserAffiliated(
        user_affiliation_ids, connector->GetDeviceAffiliationIDs(),
        account_id.GetUserEmail(), connector->GetEnterpriseDomain());
    user->SetAffiliation(is_affiliated);

    if (user->GetType() == user_manager::USER_TYPE_REGULAR) {
      if (is_affiliated) {
        AddReportingUser(account_id);
      } else {
        RemoveReportingUser(account_id);
      }
    }
  }
}

bool ChromeUserManagerImpl::ShouldReportUser(const std::string& user_id) const {
  const base::ListValue& reporting_users =
      *(GetLocalState()->GetList(kReportingUsers));
  base::StringValue user_id_value(FullyCanonicalize(user_id));
  return !(reporting_users.Find(user_id_value) == reporting_users.end());
}

void ChromeUserManagerImpl::AddReportingUser(const AccountId& account_id) {
  ListPrefUpdate users_update(GetLocalState(), kReportingUsers);
  users_update->AppendIfNotPresent(
      new base::StringValue(account_id.GetUserEmail()));
}

void ChromeUserManagerImpl::RemoveReportingUser(const AccountId& account_id) {
  ListPrefUpdate users_update(GetLocalState(), kReportingUsers);
  users_update->Remove(
      base::StringValue(FullyCanonicalize(account_id.GetUserEmail())), NULL);
}

void ChromeUserManagerImpl::UpdateLoginState(
    const user_manager::User* active_user,
    const user_manager::User* primary_user,
    bool is_current_user_owner) const {
  chrome_user_manager_util::UpdateLoginState(active_user, primary_user,
                                             is_current_user_owner);
}

bool ChromeUserManagerImpl::GetPlatformKnownUserId(
    const std::string& user_email,
    const std::string& gaia_id,
    AccountId* out_account_id) const {
  return chrome_user_manager_util::GetPlatformKnownUserId(user_email, gaia_id,
                                                          out_account_id);
}

const AccountId& ChromeUserManagerImpl::GetGuestAccountId() const {
  return login::GuestAccountId();
}

bool ChromeUserManagerImpl::IsFirstExecAfterBoot() const {
  return base::CommandLine::ForCurrentProcess()->HasSwitch(
      chromeos::switches::kFirstExecAfterBoot);
}

void ChromeUserManagerImpl::AsyncRemoveCryptohome(
    const AccountId& account_id) const {
  cryptohome::AsyncMethodCaller::GetInstance()->AsyncRemove(
      cryptohome::Identification(account_id),
      base::Bind(&OnRemoveUserComplete, account_id));
}

bool ChromeUserManagerImpl::IsGuestAccountId(
    const AccountId& account_id) const {
  return account_id == login::GuestAccountId();
}

bool ChromeUserManagerImpl::IsStubAccountId(const AccountId& account_id) const {
  return account_id == login::StubAccountId();
}

bool ChromeUserManagerImpl::IsSupervisedAccountId(
    const AccountId& account_id) const {
  return gaia::ExtractDomainName(account_id.GetUserEmail()) ==
         chromeos::login::kSupervisedUserDomain;
}

bool ChromeUserManagerImpl::HasBrowserRestarted() const {
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  return base::SysInfo::IsRunningOnChromeOS() &&
         command_line->HasSwitch(chromeos::switches::kLoginUser);
}

const gfx::ImageSkia& ChromeUserManagerImpl::GetResourceImagekiaNamed(
    int id) const {
  return *ResourceBundle::GetSharedInstance().GetImageSkiaNamed(id);
}

base::string16 ChromeUserManagerImpl::GetResourceStringUTF16(
    int string_id) const {
  return l10n_util::GetStringUTF16(string_id);
}

void ChromeUserManagerImpl::ScheduleResolveLocale(
    const std::string& locale,
    const base::Closure& on_resolved_callback,
    std::string* out_resolved_locale) const {
  BrowserThread::GetBlockingPool()->PostTaskAndReply(
      FROM_HERE,
      base::Bind(ResolveLocale, locale, base::Unretained(out_resolved_locale)),
      on_resolved_callback);
}

bool ChromeUserManagerImpl::IsValidDefaultUserImageId(int image_index) const {
  return image_index >= 0 &&
         image_index < chromeos::default_user_image::kDefaultImagesCount;
}

std::unique_ptr<user_manager::User>
ChromeUserManagerImpl::CreateUserFromDeviceLocalAccount(
    const AccountId& account_id,
    const policy::DeviceLocalAccount::Type type) const {
  std::unique_ptr<user_manager::User> user;
  switch (type) {
    case policy::DeviceLocalAccount::TYPE_PUBLIC_SESSION:
      user.reset(user_manager::User::CreatePublicAccountUser(account_id));
      break;
    case policy::DeviceLocalAccount::TYPE_KIOSK_APP:
      user.reset(user_manager::User::CreateKioskAppUser(account_id));
      break;
    default:
      NOTREACHED();
      break;
  }

  return user;
}

}  // namespace chromeos
