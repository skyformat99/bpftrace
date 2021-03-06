#include <iomanip>
#include <iostream>
#include <sstream>
#include <sys/epoll.h>

#include "bcc_syms.h"
#include "common.h"
#include "perf_reader.h"
#include "syms.h"

#include "bpftrace.h"
#include "attached_probe.h"

namespace bpftrace {

int BPFtrace::add_probe(ast::Probe &p)
{
  Probe probe;
  probe.path = p.path;
  probe.attach_point = p.attach_point;
  probe.name = p.name;
  if (p.type == "kprobe")
    probe.type = ProbeType::kprobe;
  else if (p.type == "kretprobe")
    probe.type = ProbeType::kretprobe;
  else if (p.type == "uprobe")
    probe.type = ProbeType::uprobe;
  else if (p.type == "uretprobe")
    probe.type = ProbeType::uretprobe;
  else
    return -1;
  probes_.push_back(probe);
  return 0;
}

void perf_event_printer(void *cb_cookie, void *data, int size)
{
  auto bpftrace = static_cast<BPFtrace*>(cb_cookie);
  auto fmt = static_cast<char*>(data);
  auto arg_data = static_cast<uint8_t*>(data);
  arg_data += STRING_SIZE;

  auto args = bpftrace->format_strings_[fmt];
  std::vector<uint64_t> arg_values;
  for (auto arg : args)
  {
    switch (arg.type)
    {
      case Type::integer:
        arg_values.push_back(*(uint64_t*)arg_data);
        break;
      case Type::string:
        arg_values.push_back((uint64_t)arg_data);
        break;
      default:
        abort();
    }
    arg_data +=  arg.size;
  }

  // TODO remove when \n works with the lexer
  std::string fmt_newline(fmt);
  fmt_newline += "\n";

  switch (args.size())
  {
    case 0:
      printf(fmt_newline.c_str());
      break;
    case 1:
      printf(fmt_newline.c_str(), arg_values.at(0));
      break;
    case 2:
      printf(fmt_newline.c_str(), arg_values.at(0), arg_values.at(1));
      break;
    case 3:
      printf(fmt_newline.c_str(), arg_values.at(0), arg_values.at(1),
          arg_values.at(2));
      break;
    case 4:
      printf(fmt_newline.c_str(), arg_values.at(0), arg_values.at(1),
          arg_values.at(2), arg_values.at(3));
      break;
    case 5:
      printf(fmt_newline.c_str(), arg_values.at(0), arg_values.at(1),
          arg_values.at(2), arg_values.at(3), arg_values.at(4));
      break;
    case 6:
      printf(fmt_newline.c_str(), arg_values.at(0), arg_values.at(1),
          arg_values.at(2), arg_values.at(3), arg_values.at(4), arg_values.at(5));
      break;
    default:
      abort();
  }
}

void perf_event_lost(uint64_t lost)
{
  printf("Lost %lu events\n", lost);
}

int BPFtrace::start()
{
  for (Probe &probe : probes_)
  {
    auto func = sections_.find(probe.name);
    if (func == sections_.end())
    {
      std::cerr << "Code not generated for probe: " << probe.name << std::endl;
      return -1;
    }
    try
    {
      attached_probes_.push_back(std::make_unique<AttachedProbe>(probe, func->second));
    }
    catch (std::runtime_error e)
    {
      std::cerr << e.what() << std::endl;
      return -1;
    }
  }

  int epollfd = epoll_create1(EPOLL_CLOEXEC);
  if (epollfd == -1)
  {
    std::cerr << "Failed to create epollfd" << std::endl;
    return -1;
  }

  std::vector<int> cpus = ebpf::get_online_cpus();
  for (int cpu : cpus)
  {
    int page_cnt = 8;
    void *reader = bpf_open_perf_buffer(&perf_event_printer, &perf_event_lost, this, -1, cpu, page_cnt);
    if (reader == nullptr)
    {
      std::cerr << "Failed to open perf buffer" << std::endl;
      return -1;
    }

    struct epoll_event ev = {};
    ev.events = EPOLLIN;
    ev.data.ptr = reader;
    int reader_fd = perf_reader_fd((perf_reader*)reader);

    bpf_update_elem(perf_event_map_->mapfd_, &cpu, &reader_fd, 0);
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, reader_fd, &ev) == -1)
    {
      std::cerr << "Failed to add perf reader to epoll" << std::endl;
      return -1;
    }
  }

  int ncpus = cpus.size();
  auto events = std::vector<struct epoll_event>(ncpus);
  while (true)
  {
    int ready = epoll_wait(epollfd, events.data(), ncpus, -1);
    if (ready <= 0)
    {
      return 0;
    }

    for (int i=0; i<ready; i++)
    {
      perf_reader_event_read((perf_reader*)events[i].data.ptr);
    }
  }
  return 0;
}

void BPFtrace::stop()
{
  attached_probes_.clear();
}

int BPFtrace::print_maps() const
{
  for(auto &mapmap : maps_)
  {
    Map &map = *mapmap.second.get();
    int err;
    if (map.type_.type == Type::quantize)
      err = print_map_quantize(map);
    else
      err = print_map(map);

    if (err)
      return err;
  }

  return 0;
}

int BPFtrace::print_map(Map &map) const
{
  std::vector<uint8_t> old_key;
  try
  {
    old_key = find_empty_key(map, map.key_.size());
  }
  catch (std::runtime_error &e)
  {
    std::cerr << "Error getting key for map '" << map.name_ << "': "
              << e.what() << std::endl;
    return -2;
  }
  auto key(old_key);

  while (bpf_get_next_key(map.mapfd_, old_key.data(), key.data()) == 0)
  {
    std::cout << map.name_ << map.key_.argument_value_list(*this, key) << ": ";

    auto value = std::vector<uint8_t>(map.type_.size);
    int err = bpf_lookup_elem(map.mapfd_, key.data(), value.data());
    if (err)
    {
      std::cerr << "Error looking up elem: " << err << std::endl;
      return -1;
    }
    if (map.type_.type == Type::stack)
      std::cout << get_stack(*(uint32_t*)value.data(), false, 8);
    else if (map.type_.type == Type::ustack)
      std::cout << get_stack(*(uint32_t*)value.data(), true, 8);
    else if (map.type_.type == Type::string)
      std::cout << value.data() << std::endl;
    else
      std::cout << *(int64_t*)value.data() << std::endl;

    old_key = key;
  }

  std::cout << std::endl;

  return 0;
}

int BPFtrace::print_map_quantize(Map &map) const
{
  // A quantize-map adds an extra 8 bytes onto the end of its key for storing
  // the bucket number.
  // e.g. A map defined as: @x[1, 2] = @quantize(3);
  // would actually be stored with the key: [1, 2, 3]

  std::vector<uint8_t> old_key;
  try
  {
    old_key = find_empty_key(map, map.key_.size() + 8);
  }
  catch (std::runtime_error &e)
  {
    std::cerr << "Error getting key for map '" << map.name_ << "': "
              << e.what() << std::endl;
    return -2;
  }
  auto key(old_key);

  std::map<std::vector<uint8_t>, std::vector<uint64_t>> values_by_key;

  while (bpf_get_next_key(map.mapfd_, old_key.data(), key.data()) == 0)
  {
    auto key_prefix = std::vector<uint8_t>(map.key_.size());
    int bucket = key.at(map.key_.size());

    for (size_t i=0; i<map.key_.size(); i++)
      key_prefix.at(i) = key.at(i);

    uint64_t value;
    int err = bpf_lookup_elem(map.mapfd_, key.data(), &value);
    if (err)
    {
      std::cerr << "Error looking up elem: " << err << std::endl;
      return -1;
    }

    if (values_by_key.find(key_prefix) == values_by_key.end())
    {
      // New key - create a list of buckets for it
      values_by_key[key_prefix] = std::vector<uint64_t>(65);
    }
    values_by_key[key_prefix].at(bucket) = value;

    old_key = key;
  }

  for (auto &map_elem : values_by_key)
  {
    std::cout << map.name_ << map.key_.argument_value_list(*this, map_elem.first) << ": " << std::endl;

    print_quantize(map_elem.second);

    std::cout << std::endl;
  }

  return 0;
}

int BPFtrace::print_quantize(std::vector<uint64_t> values) const
{
  int max_index = -1;
  int max_value = 0;

  for (size_t i = 0; i < values.size(); i++)
  {
    int v = values.at(i);
    if (v != 0)
      max_index = i;
    if (v > max_value)
      max_value = v;
  }

  if (max_index == -1)
    return 0;

  for (int i = 0; i <= max_index; i++)
  {
    std::ostringstream header;
    if (i == 0)
    {
      header << "[0, 1]";
    }
    else
    {
      header << "[" << quantize_index_label(i);
      header << ", " << quantize_index_label(i+1) << ")";
    }

    int max_width = 52;
    int bar_width = values.at(i)/(float)max_value*max_width;
    std::string bar(bar_width, '@');

    std::cout << std::setw(16) << std::left << header.str()
              << std::setw(8) << std::right << values.at(i)
              << " |" << std::setw(max_width) << std::left << bar << "|"
              << std::endl;
  }

  return 0;
}

std::string BPFtrace::quantize_index_label(int power) const
{
  char suffix = '\0';
  if (power >= 40)
  {
    suffix = 'T';
    power -= 40;
  }
  else if (power >= 30)
  {
    suffix = 'G';
    power -= 30;
  }
  else if (power >= 20)
  {
    suffix = 'M';
    power -= 20;
  }
  else if (power >= 10)
  {
    suffix = 'k';
    power -= 10;
  }

  std::ostringstream label;
  label << (1<<power);
  if (suffix)
    label << suffix;
  return label.str();
}

std::vector<uint8_t> BPFtrace::find_empty_key(Map &map, size_t size) const
{
  if (size == 0) size = 8;
  auto key = std::vector<uint8_t>(size);
  auto value = std::vector<uint8_t>(map.type_.size);

  if (bpf_lookup_elem(map.mapfd_, key.data(), value.data()))
    return key;

  for (auto &elem : key) elem = 0xff;
  if (bpf_lookup_elem(map.mapfd_, key.data(), value.data()))
    return key;

  for (auto &elem : key) elem = 0x55;
  if (bpf_lookup_elem(map.mapfd_, key.data(), value.data()))
    return key;

  throw std::runtime_error("Could not find empty key");
}

std::string BPFtrace::get_stack(uint32_t stackid, bool ustack, int indent) const
{
  auto stack_trace = std::vector<uint64_t>(MAX_STACK_SIZE);
  int err = bpf_lookup_elem(stackid_map_->mapfd_, &stackid, stack_trace.data());
  if (err)
  {
    std::cerr << "Error looking up stack id " << stackid << ": " << err << std::endl;
    return "";
  }

  std::ostringstream stack;
  std::string padding(indent, ' ');
  struct bcc_symbol sym;
  KSyms ksyms;

  stack << "\n";
  for (auto &addr : stack_trace)
  {
    if (addr == 0)
      break;
    if (!ustack && ksyms.resolve_addr(addr, &sym))
      stack << padding << sym.name << "+" << sym.offset << ":" << sym.module << std::endl;
    else
      stack << padding << (void*)addr << std::endl;
  }

  return stack.str();
}

} // namespace bpftrace
