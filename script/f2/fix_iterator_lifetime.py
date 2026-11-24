"""Patch only the build copy: async scan callbacks own their page buffers."""


def apply(path):
    source = path.read_text()
    start = source.index('class ConcurrentLogPageIterator {')
    end = source.index('\n};', start) + 3
    iterator = source[start:end]

    def replace(old, new, count=1):
        nonlocal iterator
        assert iterator.count(old) == count, old
        iterator = iterator.replace(old, new)

    replace('  typedef ConcurrentLogPageIterator<faster_t> log_page_iter_t;',
            '  typedef ConcurrentLogPageIterator<faster_t> log_page_iter_t;\n'
            '  using page_buffer_t = std::shared_ptr<concurrent_log_page_t[]>;')
    replace('    pages_ = new concurrent_log_page_t[num_pages];',
            '    pages_.reset(new concurrent_log_page_t[num_pages]);')
    replace('  ~ConcurrentLogPageIterator() {\n'
            '    if (pages_) {\n'
            '      delete[] pages_;\n'
            '    }\n'
            '    pages_ = nullptr;\n'
            '  }',
            '  // Epoch and I/O callbacks can finish after the scan returns.\n'
            '  // Their shared page owner must not depend on this stack object.\n'
            '  ~ConcurrentLogPageIterator() = default;')
    replace('    OnPageProcessed_Context(log_page_iter_t* iterator_,\n'
            '                            Address old_head_address_)\n'
            '      : iterator{ iterator_ }\n'
            '      , old_head_address{ old_head_address_ } {',
            '    OnPageProcessed_Context(page_buffer_t pages_, concurrent_log_page_t* page_)\n'
            '      : pages{ std::move(pages_) }, page{ page_ } {')
    replace('      : iterator{ other.iterator }\n'
            '      , old_head_address{ other.old_head_address } {',
            '      : pages{ other.pages }, page{ other.page } {')
    replace('    log_page_iter_t* iterator;\n'
            '    Address old_head_address;',
            '    page_buffer_t pages;\n'
            '    concurrent_log_page_t* page;')
    replace('    OnPageRead_Context(log_page_iter_t* iterator_, Address start_address_,',
            '    OnPageRead_Context(page_buffer_t pages_, concurrent_log_page_t* page_, Address start_address_,')
    replace('      : iterator{ iterator_ }\n'
            '      , start_address{ start_address_ }',
            '      : pages{ std::move(pages_) }, page{ page_ }\n'
            '      , start_address{ start_address_ }')
    replace('      : iterator{ other.iterator }\n'
            '      , start_address{ other.start_address }',
            '      : pages{ other.pages }, page{ other.page }\n'
            '      , start_address{ other.start_address }')
    replace('    log_page_iter_t* iterator;\n'
            '    Address start_address;',
            '    page_buffer_t pages;\n'
            '    concurrent_log_page_t* page;\n'
            '    Address start_address;')
    replace('    auto& old_page = context->iterator->Page(context->old_head_address.page());',
            '    auto& old_page = *context->page;')
    replace('    auto& page = context->iterator->Page(context->start_address.page());',
            '    auto& page = *context->page;')
    replace('      OnPageProcessed_Context context{ this, head_address };',
            '      OnPageProcessed_Context context{ pages_, &Page(head_address.page()) };')
    replace('      OnPageRead_Context context{ this, start_address, until_, bytes_to_read };',
            '      OnPageRead_Context context{ pages_, &page, start_address, until_, bytes_to_read };')
    replace('  concurrent_log_page_t* pages_;', '  page_buffer_t pages_;')
    assert 'context->iterator' not in iterator
    assert 'log_page_iter_t* iterator' not in iterator
    path.write_text(source[:start] + iterator + source[end:])
