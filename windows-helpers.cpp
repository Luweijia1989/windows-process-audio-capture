#include <Windows.h>
#include <Psapi.h>
#include <util/dstr.h>
#include <obs.h>
#include <map>
#include <list>
#include <vector>
#include <sstream>

extern "C" {
extern int s_cmp(const char *str1, const char *str2);
extern HANDLE open_process(DWORD desired_access, bool inherit_handle, DWORD process_id);
bool get_pid_exe(struct dstr *name, DWORD id)
{
	wchar_t wname[MAX_PATH];
	struct dstr temp = {0};
	bool success = false;
	HANDLE process = NULL;
	char *slash;

	process = open_process(PROCESS_QUERY_LIMITED_INFORMATION, false, id);
	if (!process)
		goto fail;

	if (!GetProcessImageFileNameW(process, wname, MAX_PATH))
		goto fail;

	dstr_from_wcs(&temp, wname);
	if (strstr(temp.array, "\\Windows\\System32") != NULL || strstr(temp.array, "Microsoft Visual Studio") != NULL)
		goto fail;

	slash = strrchr(temp.array, '\\');
	if (!slash)
		goto fail;

	dstr_copy(name, slash + 1);
	success = true;

fail:
	if (!success)
		dstr_copy(name, "unknown");

	dstr_free(&temp);
	CloseHandle(process);
	return true;
}

bool find_selectd_process(const char *process_image_name, DWORD *id, bool *changed, char *new_name)
{
	//exe:pid
	if (!process_image_name || strlen(process_image_name) == 0)
		return false;

	std::vector<std::string> strings;
	std::istringstream f(process_image_name);
	std::string s;
	while (getline(f, s, ':')) {
		strings.push_back(s);
	}
	if (strings.size() != 2)
		return false;

	DWORD cid = std::stoi(strings[1]);

	DWORD processes[1024], process_count;
	if (!EnumProcesses(processes, sizeof(processes), &process_count))
		return false;

	DWORD other_id = 0;
	bool other_found = false;
	process_count = process_count / sizeof(DWORD);
	for (unsigned int i = 0; i < process_count; i++) {
		if (processes[i] == cid) {
			*id = processes[i];
			return true;
		} else {
			if (!other_found) {
				struct dstr exe = {0};
				get_pid_exe(&exe, processes[i]);
				if (s_cmp(exe.array, strings[0].c_str()) == 0) {
					other_found = true;
					other_id = processes[i];
				}
				dstr_free(&exe);
			}
		}
	}

	if (other_found) {
		*id = other_id;
		*changed = true;
		sprintf(new_name, "%s:%d", strings[0].c_str(), other_id);
		return true;
	}

	return false;
}

void fill_process_list(obs_property_t *p)
{
	DWORD processes[1024], process_count;
	unsigned int i;

	if (!EnumProcesses(processes, sizeof(processes), &process_count))
		return;

	std::map<std::string, std::list<DWORD>> all_process;
	process_count = process_count / sizeof(DWORD);
	for (i = 0; i < process_count; i++) {
		if (processes[i] != 0) {
			struct dstr exe = {0};
			get_pid_exe(&exe, processes[i]);
			if (s_cmp(exe.array, "unknown") != 0) {
				std::list<DWORD> &one = all_process[std::string(exe.array)];
				one.push_back(processes[i]);
			}
			dstr_free(&exe);
		}
	}

	for (auto iter = all_process.begin(); iter != all_process.end(); iter++) {
		std::list<DWORD> &one = iter->second;
		for (auto iter2 = one.begin(); iter2 != one.end(); iter2++) {
			char buf[MAX_PATH] = {0};
			sprintf(buf, "%s:%d", iter->first.c_str(), *iter2);
			obs_property_list_add_string(p, buf, buf);
		}
	}
}
}
