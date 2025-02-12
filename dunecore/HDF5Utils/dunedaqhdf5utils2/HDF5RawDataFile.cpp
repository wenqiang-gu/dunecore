/**
 * This is part of the DUNE DAQ Application Framework, copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 *
 * Modified November 2022 by Tom Junk -- remove writing functionality (depended on an arbitrary
 * choice of channel maps), and change logging and exception classes.
 */

#include "dunecore/HDF5Utils/dunedaqhdf5utils2/HDF5RawDataFile.hpp"
#include "dunecore/HDF5Utils/dunedaqhdf5utils2/hdf5filelayout/Nljs.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace dunedaq {
namespace hdf5libs {

constexpr uint32_t MAX_FILELAYOUT_VERSION = 4294967295; // NOLINT(build/unsigned)

HDF5RawDataFile::~HDF5RawDataFile()
{

  // explicit destruction; not really needed, but nice to be clear...
  m_file_ptr.reset();
  m_file_layout_ptr.reset();
}


/**
 * @brief Constructor for reading a file
 */
HDF5RawDataFile::HDF5RawDataFile(const std::string& file_name)
  : m_open_flags(HighFive::File::ReadOnly)
{
  // do the file open
  try {
    m_file_ptr = std::make_unique<HighFive::File>(file_name, m_open_flags);
  } catch (std::exception const& excpt) {
    throw cet::exception("HDF5RawDataFile") << " File open failure: " << file_name << " " << excpt.what();
  }

  if (m_file_ptr->hasAttribute("recorded_size"))
    m_recorded_size = get_attribute<size_t>("recorded_size");
  else
    m_recorded_size = 0;

  read_file_layout();

  if (m_file_ptr->hasAttribute("record_type"))
    m_record_type = get_attribute<std::string>("record_type");
  else
    m_record_type = m_file_layout_ptr->get_record_name_prefix();

  check_file_layout();

  // HDF5SourceIDHandler operations need to come *after* read_file_layout()
  // because they count on the filelayout_version, which is set in read_file_layout().
  HDF5SourceIDHandler sid_handler(get_version());
  sid_handler.fetch_file_level_geo_id_info(*m_file_ptr, m_file_level_source_id_geo_id_map);
}

void
HDF5RawDataFile::read_file_layout()
{
  hdf5filelayout::FileLayoutParams fl_params;
  uint32_t version = 0; // NOLINT(build/unsigned)

  std::string fl_str;
  try {
    fl_str = get_attribute<std::string>("filelayout_params");
    hdf5filelayout::data_t fl_json = nlohmann::json::parse(fl_str);
    hdf5filelayout::from_json(fl_json, fl_params);

    version = get_attribute<uint32_t>("filelayout_version"); // NOLINT(build/unsigned)

  } catch (cet::exception const&) { 
    MF_LOG_INFO("HDF5RawDataFile.cpp") << "Missing File Layout " << version; 
  }

  // now reset the HDF5Filelayout object
  m_file_layout_ptr.reset(new HDF5FileLayout(fl_params, version));
}

void
HDF5RawDataFile::check_file_layout()
{
  if (get_version() < 2)
    return;

  std::string record_type = get_attribute<std::string>("record_type");
  if (record_type.compare(m_file_layout_ptr->get_record_name_prefix()) != 0)
    throw cet::exception("HDF5RawDataFile.cpp") << "Bad Record Type: " << record_type << " " << m_file_layout_ptr->get_record_name_prefix();
}

void
HDF5RawDataFile::check_record_type(std::string rt_name)
{
  if (get_version() < 2)
    return;

  if (m_file_layout_ptr->get_record_name_prefix().compare(rt_name) != 0)
    throw cet::exception("HDF5RawDataFile.cpp") << "Wrong Record Type Requested: " << rt_name << " " << m_file_layout_ptr->get_record_name_prefix();
}

// HDF5 Utility function to recursively traverse a file
void
HDF5RawDataFile::explore_subgroup(const HighFive::Group& parent_group,
                                  std::string relative_path,
                                  std::vector<std::string>& path_list)
{
  if (relative_path.size() > 0 && relative_path.compare(relative_path.size() - 1, 1, "/") == 0)
    relative_path.pop_back();

  std::vector<std::string> childNames = parent_group.listObjectNames();

  for (auto& child_name : childNames) {
    std::string full_path = relative_path + "/" + child_name;
    HighFive::ObjectType child_type = parent_group.getObjectType(child_name);

    if (child_type == HighFive::ObjectType::Dataset) {
      path_list.push_back(full_path);
    } else if (child_type == HighFive::ObjectType::Group) {
      HighFive::Group child_group = parent_group.getGroup(child_name);
      // start the recusion
      std::string new_path = relative_path + "/" + child_name;
      explore_subgroup(child_group, new_path, path_list);
    }
  }
}

void
HDF5RawDataFile::add_record_level_info_to_caches_if_needed(record_id_t rid)
{
  // we should probably check that all relevant caches have an entry for the
  // specified record ID, but we will just check one, in the interest of
  // performance, and trust the "else" part of this routine to fill in *all*
  // of the appropriate caches
  if (m_source_id_path_cache.count(rid) != 0) {
    return;
  }

  // create the handler to do the work
  HDF5SourceIDHandler sid_handler(get_version());

  // determine the HDF5 Group that corresponds to the specified record
  std::string record_level_group_name = m_file_layout_ptr->get_record_number_string(rid.first, rid.second);
  HighFive::Group record_group = m_file_ptr->getGroup(record_level_group_name);
  if (!record_group.isValid()) {
    throw cet::exception("HDF5RawDataFile.cpp") << "Invalid HDF5 Group: " << record_level_group_name;
  }

  // start with a copy of the file-level source-id-to-geo-id map and give the
  // handler an opportunity to add any record-level additions
  HDF5SourceIDHandler::source_id_geo_id_map_t local_source_id_geo_id_map = m_file_level_source_id_geo_id_map;
  sid_handler.fetch_record_level_geo_id_info(record_group, local_source_id_geo_id_map);

  // fetch the record-level source-id-to-path map
  HDF5SourceIDHandler::source_id_path_map_t source_id_path_map;
  sid_handler.fetch_source_id_path_info(record_group, source_id_path_map);

  // fetch the record-level fragment-type-to-source-id map
  HDF5SourceIDHandler::fragment_type_source_id_map_t fragment_type_source_id_map;
  sid_handler.fetch_fragment_type_source_id_info(record_group, fragment_type_source_id_map);

  // fetch the record-level subdetector-to-source-id map
  HDF5SourceIDHandler::subdetector_source_id_map_t subdetector_source_id_map;
  sid_handler.fetch_subdetector_source_id_info(record_group, subdetector_source_id_map);

  // loop through the source-id-to-path map to create various lists of SourceIDs in the record
  daqdataformats::SourceID rh_sid = sid_handler.fetch_record_header_source_id(record_group);
  std::set<daqdataformats::SourceID> full_source_id_set;
  std::set<daqdataformats::SourceID> fragment_source_id_set;
  HDF5SourceIDHandler::subsystem_source_id_map_t subsystem_source_id_map;
  for (auto const& source_id_path : source_id_path_map) {
    full_source_id_set.insert(source_id_path.first);
    if (source_id_path.first != rh_sid) {
      fragment_source_id_set.insert(source_id_path.first);
    }
    HDF5SourceIDHandler::add_subsystem_source_id_to_map(
      subsystem_source_id_map, source_id_path.first.subsystem, source_id_path.first);
  }

  // note that even if the "fetch" methods above fail to add anything to the specified
  // maps, the maps will still be valid (though, possibly empty), and once we add them
  // to the caches here, we will be assured that lookups from the caches will not fail.
  m_source_id_cache[rid] = full_source_id_set;
  m_record_header_source_id_cache[rid] = rh_sid;
  m_fragment_source_id_cache[rid] = fragment_source_id_set;
  m_source_id_geo_id_cache[rid] = local_source_id_geo_id_map;
  m_source_id_path_cache[rid] = source_id_path_map;
  m_subsystem_source_id_cache[rid] = subsystem_source_id_map;
  m_fragment_type_source_id_cache[rid] = fragment_type_source_id_map;
  m_subdetector_source_id_cache[rid] = subdetector_source_id_map;
}

/**
 * @brief Return a vector of dataset names
 */
std::vector<std::string>
HDF5RawDataFile::get_dataset_paths(std::string top_level_group_name)
{
  if (top_level_group_name.empty())
    top_level_group_name = m_file_ptr->getPath();

  // Vector containing the path list to the HDF5 datasets
  std::vector<std::string> path_list;

  HighFive::Group parent_group = m_file_ptr->getGroup(top_level_group_name);
  if (!parent_group.isValid())
    throw cet::exception("HDF5RawDataFile.cpp") << "Invalid HDF5 Group: " << top_level_group_name;

  explore_subgroup(parent_group, top_level_group_name, path_list);

  return path_list;
}

/**
 * @brief Return all of the record numbers in the file.
 */
HDF5RawDataFile::record_id_set // NOLINT(build/unsigned)
HDF5RawDataFile::get_all_record_ids()
{
  if (!m_all_record_ids_in_file.empty())
    return m_all_record_ids_in_file;

  // records are at the top level

  HighFive::Group parent_group = m_file_ptr->getGroup(m_file_ptr->getPath());

  std::vector<std::string> childNames = parent_group.listObjectNames();
  const std::string record_prefix = m_file_layout_ptr->get_record_name_prefix();
  const size_t record_prefix_size = record_prefix.size();

  for (auto const& name : childNames) {
    auto loc = name.find(record_prefix);

    if (loc == std::string::npos)
      continue;

    auto rec_num_string = name.substr(loc + record_prefix_size);

    loc = rec_num_string.find(".");
    if (loc == std::string::npos) {
      m_all_record_ids_in_file.insert(std::make_pair(std::stoi(rec_num_string), 0));
    } else {
      auto seq_num_string = rec_num_string.substr(loc + 1);
      rec_num_string.resize(loc); // remove anything from '.' onwards
      m_all_record_ids_in_file.insert(std::make_pair(std::stoi(rec_num_string), std::stoi(seq_num_string)));
    }

  } // end loop over childNames

  return m_all_record_ids_in_file;
}

std::set<uint64_t>
HDF5RawDataFile::get_all_record_numbers() // NOLINT(build/unsigned)
{
  MF_LOG_WARNING("HDF5RawDataFile.cpp") << "Deprecated usage, get_all_record_numbers().  Use get_all_record_ids(),  which returns a record_number,sequence_number pair.";

  std::set<uint64_t> record_numbers; // NOLINT(build/unsigned)
  for (auto const& rid : get_all_record_ids())
    record_numbers.insert(rid.first);

  return record_numbers;
}

HDF5RawDataFile::record_id_set
HDF5RawDataFile::get_all_trigger_record_ids()
{
  check_record_type("TriggerRecord");
  return get_all_record_ids();
}

std::set<daqdataformats::trigger_number_t>
HDF5RawDataFile::get_all_trigger_record_numbers()
{
  MF_LOG_WARNING("HDF5RawDataFile.cpp") << "Deprecated usage, get_all_trigger_record_numbers().  Use get_all_trigger_record_ids(),  which returns a record_number,sequence_number pair.";

  return get_all_record_numbers();
}

HDF5RawDataFile::record_id_set
HDF5RawDataFile::get_all_timeslice_ids()
{
  check_record_type("TimeSlice");
  return get_all_record_ids();
}

std::set<daqdataformats::timeslice_number_t>
HDF5RawDataFile::get_all_timeslice_numbers()
{
  check_record_type("TimeSlice");
  return get_all_record_numbers();
}

/**
 * @brief Return a vector of dataset names that correspond to record headers
 */
std::vector<std::string>
HDF5RawDataFile::get_record_header_dataset_paths()
{

  std::vector<std::string> rec_paths;

  if (get_version() >= 2) {
    for (auto const& rec_id : get_all_record_ids())
      rec_paths.push_back(get_record_header_dataset_path(rec_id));
  } else {
    for (auto const& path : get_dataset_paths()) {
      if (path.find(m_file_layout_ptr->get_record_header_dataset_name()) != std::string::npos) {
        rec_paths.push_back(path);
      }
    }
  }

  return rec_paths;
}

std::vector<std::string>
HDF5RawDataFile::get_trigger_record_header_dataset_paths()
{
  check_record_type("TriggerRecord");
  return get_record_header_dataset_paths();
}

std::vector<std::string>
HDF5RawDataFile::get_timeslice_header_dataset_paths()
{
  check_record_type("TimeSlice");
  return get_record_header_dataset_paths();
}

std::string
HDF5RawDataFile::get_record_header_dataset_path(const record_id_t& rid)
{
  auto rec_id = get_all_record_ids().find(rid);
  if (rec_id == get_all_record_ids().end())
    throw cet::exception("HDF5RawDataFile.cpp") << "Record ID Not Found: " << rid.first << " " << rid.second;

  if (get_version() <= 2) {
    return (m_file_ptr->getPath() + m_file_layout_ptr->get_record_header_path(rid.first, rid.second));
  } else {
    daqdataformats::SourceID source_id = get_record_header_source_id(rid);
    return m_source_id_path_cache[rid][source_id];
  }
}

std::string
HDF5RawDataFile::get_record_header_dataset_path(const uint64_t rec_num, // NOLINT (build/unsigned)
                                                const daqdataformats::sequence_number_t seq_num)
{
  return get_record_header_dataset_path(std::make_pair(rec_num, seq_num));
}

std::string
HDF5RawDataFile::get_trigger_record_header_dataset_path(const record_id_t& rid)
{
  check_record_type("TriggerRecord");
  return get_record_header_dataset_path(rid);
}

std::string
HDF5RawDataFile::get_trigger_record_header_dataset_path(const daqdataformats::trigger_number_t trig_num,
                                                        const daqdataformats::sequence_number_t seq_num)
{
  check_record_type("TriggerRecord");
  return get_record_header_dataset_path(trig_num, seq_num);
}

std::string
HDF5RawDataFile::get_timeslice_header_dataset_path(const record_id_t& rid)
{
  check_record_type("TimeSlice");
  return get_record_header_dataset_path(rid.first, 0);
}

std::string
HDF5RawDataFile::get_timeslice_header_dataset_path(const daqdataformats::timeslice_number_t ts_num)
{
  check_record_type("TimeSlice");
  return get_record_header_dataset_path(ts_num);
}

/**
 * @brief Return a vector of dataset names that correspond to Fragemnts
 * Note: this gets all datsets, and then removes those that look like TriggerRecordHeader ones
 *       one could instead loop through all system types and ask for appropriate datsets in those
 *       however, probably that's more time consuming
 */
std::vector<std::string>
HDF5RawDataFile::get_all_fragment_dataset_paths()
{
  std::vector<std::string> frag_paths;

  for (auto const& path : get_dataset_paths()) {
    if (path.find(m_file_layout_ptr->get_record_header_dataset_name()) == std::string::npos)
      frag_paths.push_back(path);
  }

  return frag_paths;
}

// get all fragment dataset paths for given record ID
std::vector<std::string>
HDF5RawDataFile::get_fragment_dataset_paths(const record_id_t& rid)
{
  auto rec_id = get_all_record_ids().find(rid);
  if (rec_id == get_all_record_ids().end())
    throw cet::exception("HDF5RawDataFile.cpp") << "Record ID Not Found: " << rid.first << " " << rid.second;

  std::vector<std::string> frag_paths;
  if (get_version() <= 2) {
    std::string record_group_path =
      m_file_ptr->getPath() + m_file_layout_ptr->get_record_number_string(rid.first, rid.second);

    for (auto const& path : get_dataset_paths(record_group_path)) {
      if (path.find(m_file_layout_ptr->get_record_header_dataset_name()) == std::string::npos)
        frag_paths.push_back(path);
    }
  } else {
    std::set<daqdataformats::SourceID> source_id_list = get_fragment_source_ids(rid);
    for (auto const& source_id : source_id_list) {
      frag_paths.push_back(m_source_id_path_cache[rid][source_id]);
    }
  }
  return frag_paths;
}

// get all fragment dataset paths for given record ID
std::vector<std::string>
HDF5RawDataFile::get_fragment_dataset_paths(const uint64_t rec_num, // NOLINT (build/unsigned)
                                            const daqdataformats::sequence_number_t seq_num)
{
  return get_fragment_dataset_paths(std::make_pair(rec_num, seq_num));
}

// get all fragment dataset paths for a Subsystem
std::vector<std::string>
HDF5RawDataFile::get_fragment_dataset_paths(const daqdataformats::SourceID::Subsystem subsystem)
{
  std::vector<std::string> frag_paths;
  for (auto const& rid : get_all_record_ids()) {
    if (get_version() <= 2) {
      auto datasets = get_dataset_paths(m_file_ptr->getPath() +
                                        m_file_layout_ptr->get_fragment_type_path(rid.first, rid.second, subsystem));
      frag_paths.insert(frag_paths.end(), datasets.begin(), datasets.end());
    } else {
      std::set<daqdataformats::SourceID> source_id_list = get_source_ids_for_subsystem(rid, subsystem);
      for (auto const& source_id : source_id_list) {
        frag_paths.push_back(m_source_id_path_cache[rid][source_id]);
      }
    }
  }
  return frag_paths;
}

std::vector<std::string>
HDF5RawDataFile::get_fragment_dataset_paths(const std::string& subsystem_name)
{
  daqdataformats::SourceID::Subsystem subsystem = daqdataformats::SourceID::string_to_subsystem(subsystem_name);
  return get_fragment_dataset_paths(subsystem);
}

std::vector<std::string>
HDF5RawDataFile::get_fragment_dataset_paths(const record_id_t& rid, const daqdataformats::SourceID::Subsystem subsystem)
{
  auto rec_id = get_all_record_ids().find(rid);
  if (rec_id == get_all_record_ids().end())
    throw cet::exception("HDF5RawDataFile.cpp") << "Record ID Not Found: " << rid.first << " " << rid.second;

  if (get_version() <= 2) {
    return get_dataset_paths(m_file_ptr->getPath() +
                             m_file_layout_ptr->get_fragment_type_path(rid.first, rid.second, subsystem));
  } else {
    std::vector<std::string> frag_paths;
    std::set<daqdataformats::SourceID> source_id_list = get_source_ids_for_subsystem(rid, subsystem);
    for (auto const& source_id : source_id_list) {
      frag_paths.push_back(m_source_id_path_cache[rid][source_id]);
    }
    return frag_paths;
  }
}

std::vector<std::string>
HDF5RawDataFile::get_fragment_dataset_paths(const record_id_t& rid, const std::string& subsystem_name)
{
  daqdataformats::SourceID::Subsystem subsystem = daqdataformats::SourceID::string_to_subsystem(subsystem_name);
  return get_fragment_dataset_paths(rid, subsystem);
}

#if 0
// get all fragment dataset paths for a SourceID
std::vector<std::string>
HDF5RawDataFile::get_fragment_dataset_paths(const daqdataformats::SourceID& source_id)
{
  std::vector<std::string> frag_paths;

  for (auto const& rid : get_all_record_ids())
    frag_paths.push_back(m_file_ptr->getPath() +
                         m_file_layout_ptr->get_fragment_path(rid.first, rid.second, source_id));

  return frag_paths;
}

std::vector<std::string>
HDF5RawDataFile::get_fragment_dataset_paths(const daqdataformats::SourceID::Subsystem type,
                                               const uint32_t id) // NOLINT(build/unsigned)
{
  return get_fragment_dataset_paths(daqdataformats::SourceID(type, source_id));
}
std::vector<std::string>
HDF5RawDataFile::get_fragment_dataset_paths(const std::string& typestring,
                                               const uint32_t id) // NOLINT(build/unsigned)
{
  return get_fragment_dataset_paths(
    daqdataformats::SourceID(daqdataformats::SourceID::string_to_subsystem(typestring), source_id));
}

std::set<daqdataformats::SourceID>
HDF5RawDataFile::get_source_ids(std::vector<std::string> const& frag_dataset_paths)
{
  std::set<daqdataformats::SourceID> source_ids;
  std::vector<std::string> path_elements;
  std::string s;
  for (auto const& frag_dataset : frag_dataset_paths) {
    path_elements.clear();
    std::istringstream iss(frag_dataset);
    while (std::getline(iss, s, '/')) {
      if (s.size() > 0)
        path_elements.push_back(s);
    }
    source_ids.insert(m_file_layout_ptr->get_source_id_from_path_elements(path_elements));
  }

  return source_ids;
}
#endif

std::set<uint64_t> // NOLINT(build/unsigned)
HDF5RawDataFile::get_all_geo_ids()
{
  std::set<uint64_t> set_of_geo_ids;
  // 13-Sep-2022, KAB
  // It would be safer, but slower, to fetch all of the geo_ids from the 
  // individual records, and we'll go with faster, for now.  If/when we
  // change the way that we determine the file-level and record-level
  // source_id-to-geo_id maps, we may need to change this code.
  for (auto const& map_entry : m_file_level_source_id_geo_id_map) {
    for (auto const& geo_id : map_entry.second) {
      set_of_geo_ids.insert(geo_id);
    }
  }
  return set_of_geo_ids;
}

std::set<uint64_t> // NOLINT(build/unsigned)
HDF5RawDataFile::get_geo_ids(const record_id_t& rid)
{
  auto rec_id = get_all_record_ids().find(rid);
  if (rec_id == get_all_record_ids().end())
    throw cet::exception("HDF5RawDataFile.cpp") << "Record ID Not Found: " << rid.first << " " << rid.second;

  add_record_level_info_to_caches_if_needed(rid);

  std::set<uint64_t> set_of_geo_ids;
  for (auto const& map_entry : m_source_id_geo_id_cache[rid]) {
    for (auto const& geo_id : map_entry.second) {
      set_of_geo_ids.insert(geo_id);
    }
  }
  return set_of_geo_ids;
}

std::set<uint64_t> // NOLINT(build/unsigned)
HDF5RawDataFile::get_geo_ids_for_subdetector(const record_id_t& rid,
                                             const detdataformats::DetID::Subdetector subdet)
{
  auto rec_id = get_all_record_ids().find(rid);
  if (rec_id == get_all_record_ids().end())
    throw cet::exception("HDF5RawDataFile.cpp") << "Record ID Not Found: " << rid.first << " " << rid.second;

  add_record_level_info_to_caches_if_needed(rid);

  std::set<uint64_t> set_of_geo_ids;
  for (auto const& map_entry : m_source_id_geo_id_cache[rid]) {
    for (auto const& geo_id : map_entry.second) {

      // auto geo_info = detchannelmaps::HardwareMapService::parse_geo_id(geo_id);
      // had been a test on geo_info.det_id, but instead move the functionality here.

      uint16_t geoinfodetid = 0xffff & geo_id;
      if (geoinfodetid == static_cast<uint16_t>(subdet)) {
        set_of_geo_ids.insert(geo_id);
      }
    }
  }
  return set_of_geo_ids;
}


// get all SourceIDs for given record ID
std::set<daqdataformats::SourceID>
HDF5RawDataFile::get_source_ids(const record_id_t& rid)
{
  auto rec_id = get_all_record_ids().find(rid);
  if (rec_id == get_all_record_ids().end())
    throw cet::exception("HDF5RawDataFile.cpp") << "Record ID Not Found: " << rid.first << " " << rid.second;

  add_record_level_info_to_caches_if_needed(rid);

  return m_source_id_cache[rid];
}

daqdataformats::SourceID
HDF5RawDataFile::get_record_header_source_id(const record_id_t& rid)
{
  auto rec_id = get_all_record_ids().find(rid);
  if (rec_id == get_all_record_ids().end())
    throw cet::exception("HDF5RawDataFile.cpp") << "Record ID Not Found: " << rid.first << " " << rid.second;

  add_record_level_info_to_caches_if_needed(rid);

  return m_record_header_source_id_cache[rid];
}

std::set<daqdataformats::SourceID>
HDF5RawDataFile::get_fragment_source_ids(const record_id_t& rid)
{
  auto rec_id = get_all_record_ids().find(rid);
  if (rec_id == get_all_record_ids().end())
    throw cet::exception("HDF5RawDataFile.cpp") << "Record ID Not Found: " << rid.first << " " << rid.second;

  add_record_level_info_to_caches_if_needed(rid);

  return m_fragment_source_id_cache[rid];
}

std::set<daqdataformats::SourceID>
HDF5RawDataFile::get_source_ids_for_subsystem(const record_id_t& rid,
                                              const daqdataformats::SourceID::Subsystem subsystem)
{
  auto rec_id = get_all_record_ids().find(rid);
  if (rec_id == get_all_record_ids().end())
    throw cet::exception("HDF5RawDataFile.cpp") << "Record ID Not Found: " << rid.first << " " << rid.second;

  add_record_level_info_to_caches_if_needed(rid);

  return m_subsystem_source_id_cache[rid][subsystem];
}

std::set<daqdataformats::SourceID>
HDF5RawDataFile::get_source_ids_for_fragment_type(const record_id_t& rid, const daqdataformats::FragmentType frag_type)
{
  auto rec_id = get_all_record_ids().find(rid);
  if (rec_id == get_all_record_ids().end())
    throw cet::exception("HDF5RawDataFile.cpp") << "Record ID Not Found: " << rid.first << " " << rid.second;

  add_record_level_info_to_caches_if_needed(rid);

  return m_fragment_type_source_id_cache[rid][frag_type];
}

std::set<daqdataformats::SourceID>
HDF5RawDataFile::get_source_ids_for_subdetector(const record_id_t& rid, const detdataformats::DetID::Subdetector subdet)
{
  auto rec_id = get_all_record_ids().find(rid);
  if (rec_id == get_all_record_ids().end())
    throw cet::exception("HDF5RawDataFile.cpp") << "Record ID Not Found: " << rid.first << " " << rid.second;

  add_record_level_info_to_caches_if_needed(rid);

  return m_subdetector_source_id_cache[rid][subdet];
}

std::unique_ptr<char[]>
HDF5RawDataFile::get_dataset_raw_data(const std::string& dataset_path)
{

  HighFive::Group parent_group = m_file_ptr->getGroup("/");
  HighFive::DataSet data_set = parent_group.getDataSet(dataset_path);

  if (!data_set.isValid())
    throw cet::exception("HDF5RawDataFile.cpp") << "Invalid HDF5 Dataset: " << dataset_path << " " << get_file_name();

  size_t data_size = data_set.getStorageSize();

  auto membuffer = std::make_unique<char[]>(data_size);
  data_set.read(membuffer.get());
  return membuffer;
}

std::unique_ptr<daqdataformats::Fragment>
HDF5RawDataFile::get_frag_ptr(const std::string& dataset_name)
{
  auto membuffer = get_dataset_raw_data(dataset_name);
  auto frag_ptr = std::make_unique<daqdataformats::Fragment>(
    membuffer.release(), dunedaq::daqdataformats::Fragment::BufferAdoptionMode::kTakeOverBuffer);
  return frag_ptr;
}

std::unique_ptr<daqdataformats::Fragment>
HDF5RawDataFile::get_frag_ptr(const record_id_t& rid, const daqdataformats::SourceID& source_id)
{
  if (get_version() < 2)
    throw cet::exception("HDF5RawDataFile.cpp") << "Incompatible File Layout Version: " <<  get_version() << " 2 " << MAX_FILELAYOUT_VERSION;

  auto rec_id = get_all_record_ids().find(rid);
  if (rec_id == get_all_record_ids().end())
    throw cet::exception("HDF5RawDataFile.cpp") << "Record ID Not Found: " << rid.first << " " << rid.second;

  add_record_level_info_to_caches_if_needed(rid);

  return get_frag_ptr(m_source_id_path_cache[rid][source_id]);
}

std::unique_ptr<daqdataformats::Fragment>
HDF5RawDataFile::get_frag_ptr(const uint64_t rec_num, // NOLINT(build/unsigned)
                              const daqdataformats::sequence_number_t seq_num,
                              const daqdataformats::SourceID& source_id)
{
  record_id_t rid = std::make_pair(rec_num, seq_num);
  return get_frag_ptr(rid, source_id);
}

std::unique_ptr<daqdataformats::Fragment>
HDF5RawDataFile::get_frag_ptr(const record_id_t& rid,
                              const daqdataformats::SourceID::Subsystem type,
                              const uint32_t id) // NOLINT(build/unsigned)
{
  daqdataformats::SourceID source_id(type, id);
  return get_frag_ptr(rid, source_id);
}

std::unique_ptr<daqdataformats::Fragment>
HDF5RawDataFile::get_frag_ptr(const uint64_t rec_num, // NOLINT(build/unsigned)
                              const daqdataformats::sequence_number_t seq_num,
                              const daqdataformats::SourceID::Subsystem type,
                              const uint32_t id) // NOLINT(build/unsigned)
{
  record_id_t rid = std::make_pair(rec_num, seq_num);
  daqdataformats::SourceID source_id(type, id);
  return get_frag_ptr(rid, source_id);
}

std::unique_ptr<daqdataformats::Fragment>
HDF5RawDataFile::get_frag_ptr(const record_id_t& rid,
                              const std::string& typestring,
                              const uint32_t id) // NOLINT(build/unsigned)
{
  daqdataformats::SourceID source_id(daqdataformats::SourceID::string_to_subsystem(typestring), id);
  return get_frag_ptr(rid, source_id);
}

std::unique_ptr<daqdataformats::Fragment>
HDF5RawDataFile::get_frag_ptr(const uint64_t rec_num, // NOLINT(build/unsigned)
                              const daqdataformats::sequence_number_t seq_num,
                              const std::string& typestring,
                              const uint32_t id) // NOLINT(build/unsigned)
{
  record_id_t rid = std::make_pair(rec_num, seq_num);
  daqdataformats::SourceID source_id(daqdataformats::SourceID::string_to_subsystem(typestring), id);
  return get_frag_ptr(rid, source_id);
}

std::unique_ptr<daqdataformats::Fragment>
HDF5RawDataFile::get_frag_ptr(const record_id_t& rid,
                              const uint64_t geo_id) // NOLINT(build/unsigned)
{
  daqdataformats::SourceID sid = get_source_id_for_geo_id(rid, geo_id);
  return get_frag_ptr(rid, sid);
}

std::unique_ptr<daqdataformats::Fragment>
HDF5RawDataFile::get_frag_ptr(const uint64_t rec_num, // NOLINT(build/unsigned)
                              const daqdataformats::sequence_number_t seq_num,
                              const uint64_t geo_id) // NOLINT(build/unsigned)
{
  record_id_t rid = std::make_pair(rec_num, seq_num);
  return get_frag_ptr(rid, geo_id);
}

std::unique_ptr<daqdataformats::TriggerRecordHeader>
HDF5RawDataFile::get_trh_ptr(const std::string& dataset_name)
{
  auto membuffer = get_dataset_raw_data(dataset_name);
  auto trh_ptr = std::make_unique<daqdataformats::TriggerRecordHeader>(membuffer.release(), true);
  return trh_ptr;
}

std::unique_ptr<daqdataformats::TriggerRecordHeader>
HDF5RawDataFile::get_trh_ptr(const record_id_t& rid)
{
  if (get_version() < 2)
    throw cet::exception("HDF5RawDataFile.cpp") << "Incompatible File Layout Version: " <<  get_version() << " 2 " << MAX_FILELAYOUT_VERSION;

  auto rec_id = get_all_record_ids().find(rid);
  if (rec_id == get_all_record_ids().end())
    throw cet::exception("HDF5RawDataFile.cpp") << "Record ID Not Found: " << rid.first << " " << rid.second;

  add_record_level_info_to_caches_if_needed(rid);

  daqdataformats::SourceID rh_source_id = m_record_header_source_id_cache[rid];
  return get_trh_ptr(m_source_id_path_cache[rid][rh_source_id]);
}

std::unique_ptr<daqdataformats::TimeSliceHeader>
HDF5RawDataFile::get_tsh_ptr(const std::string& dataset_name)
{
  auto membuffer = get_dataset_raw_data(dataset_name);
  auto tsh_ptr = std::make_unique<daqdataformats::TimeSliceHeader>(
    *(reinterpret_cast<daqdataformats::TimeSliceHeader*>(membuffer.release()))); // NOLINT
  return tsh_ptr;
}

std::unique_ptr<daqdataformats::TimeSliceHeader>
HDF5RawDataFile::get_tsh_ptr(const record_id_t& rid)
{
  if (get_version() < 2)
    throw cet::exception("HDF5RawDataFile.cpp") << "Incompatible File Layout Version: " <<  get_version() << " 2 " << MAX_FILELAYOUT_VERSION;

  auto rec_id = get_all_record_ids().find(rid);
  if (rec_id == get_all_record_ids().end())
    throw cet::exception("HDF5RawDataFile.cpp") << "Record ID Not Found: " << rid.first << " " << rid.second;

  add_record_level_info_to_caches_if_needed(rid);

  daqdataformats::SourceID rh_source_id = m_record_header_source_id_cache[rid];
  return get_tsh_ptr(m_source_id_path_cache[rid][rh_source_id]);
}

daqdataformats::TriggerRecord
HDF5RawDataFile::get_trigger_record(const record_id_t& rid)
{
  daqdataformats::TriggerRecord trigger_record(*get_trh_ptr(rid));
  for (auto const& frag_path : get_fragment_dataset_paths(rid)) {
    trigger_record.add_fragment(get_frag_ptr(frag_path));
  }

  return trigger_record;
}

daqdataformats::TimeSlice
HDF5RawDataFile::get_timeslice(const daqdataformats::timeslice_number_t ts_num)
{
  daqdataformats::TimeSlice timeslice(*get_tsh_ptr(ts_num));
  for (auto const& frag_path : get_fragment_dataset_paths(ts_num)) {
    timeslice.add_fragment(get_frag_ptr(frag_path));
  }

  return timeslice;
}

std::vector<uint64_t> // NOLINT(build/unsigned)
HDF5RawDataFile::get_geo_ids_for_source_id(const record_id_t& rid, const daqdataformats::SourceID& source_id)
{
  auto rec_id = get_all_record_ids().find(rid);
  if (rec_id == get_all_record_ids().end())
    throw cet::exception("HDF5RawDataFile.cpp") << "Record ID Not Found: " << rid.first << " " << rid.second;

  add_record_level_info_to_caches_if_needed(rid);

  return m_source_id_geo_id_cache[rid][source_id];
}

daqdataformats::SourceID
HDF5RawDataFile::get_source_id_for_geo_id(const record_id_t& rid,
                                          const uint64_t requested_geo_id) // NOLINT(build/unsigned)
{
  auto rec_id = get_all_record_ids().find(rid);
  if (rec_id == get_all_record_ids().end())
    throw cet::exception("HDF5RawDataFile.cpp") << "Record ID Not Found: " << rid.first << " " << rid.second;

  add_record_level_info_to_caches_if_needed(rid);

  // if we want to make this faster, we could build a reverse lookup cache in
  // add_record_level_info_to_caches_if_needed() and just look up the requested geo_id here
  for (auto const& map_entry : m_source_id_geo_id_cache[rid]) {
    auto geoid_list = map_entry.second;
    for (auto const& geoid_from_list : geoid_list) {
      if (geoid_from_list == requested_geo_id) {
        return map_entry.first;
      }
    }
  }

  daqdataformats::SourceID empty_sid;
  return empty_sid;
}

} // namespace hdf5libs
} // namespace dunedaq
