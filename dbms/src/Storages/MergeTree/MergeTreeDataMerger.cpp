#include <DB/Storages/MergeTree/MergeTreeDataMerger.h>
#include <DB/Storages/MergeTree/MergeTreeBlockInputStream.h>
#include <DB/Storages/MergeTree/MergedBlockOutputStream.h>
#include <DB/DataStreams/ExpressionBlockInputStream.h>
#include <DB/DataStreams/MergingSortedBlockInputStream.h>
#include <DB/DataStreams/CollapsingSortedBlockInputStream.h>
#include <DB/DataStreams/SummingSortedBlockInputStream.h>


namespace DB
{

/// Не будем соглашаться мерджить куски, если места на диске менее чем во столько раз больше суммарного размера кусков.
static const double DISK_USAGE_COEFFICIENT_TO_SELECT = 1.6;

/// Объединяя куски, зарезервируем столько места на диске. Лучше сделать немного меньше, чем DISK_USAGE_COEFFICIENT_TO_SELECT,
/// потому что между выбором кусков и резервированием места места может стать немного меньше.
static const double DISK_USAGE_COEFFICIENT_TO_RESERVE = 1.4;


/// Выбираем отрезок из не более чем max_parts_to_merge_at_once кусков так, чтобы максимальный размер был меньше чем в max_size_ratio_to_merge_parts раз больше суммы остальных.
/// Это обеспечивает в худшем случае время O(n log n) на все слияния, независимо от выбора сливаемых кусков, порядка слияния и добавления.
/// При max_parts_to_merge_at_once >= log(max_rows_to_merge_parts/index_granularity)/log(max_size_ratio_to_merge_parts),
/// несложно доказать, что всегда будет что сливать, пока количество кусков больше
/// log(max_rows_to_merge_parts/index_granularity)/log(max_size_ratio_to_merge_parts)*(количество кусков размером больше max_rows_to_merge_parts).
/// Дальше эвристики.
/// Будем выбирать максимальный по включению подходящий отрезок.
/// Из всех таких выбираем отрезок с минимальным максимумом размера.
/// Из всех таких выбираем отрезок с минимальным минимумом размера.
/// Из всех таких выбираем отрезок с максимальной длиной.
/// Дополнительно:
/// 1) с 1:00 до 5:00 ограничение сверху на размер куска в основном потоке увеличивается в несколько раз
/// 2) в зависимоти от возраста кусков меняется допустимая неравномерность при слиянии
/// 3) Молодые куски крупного размера (примерно больше 1 Гб) можно сливать не меньше чем по три
/// 4) Если в одном из потоков идет мердж крупных кусков, то во втором сливать только маленькие кусочки
/// 5) С ростом логарифма суммарного размера кусочков в мердже увеличиваем требование сбалансированности

bool MergeTreeDataMerger::selectPartsToMerge(MergeTreeData::DataPartsVector & parts, size_t available_disk_space,
	bool merge_anything_for_old_months, bool aggressive, bool only_small, const AllowedMergingPredicate & can_merge)
{
	LOG_DEBUG(log, "Selecting parts to merge");

	MergeTreeData::DataParts data_parts = data.getDataParts();

	DateLUTSingleton & date_lut = DateLUTSingleton::instance();

	size_t min_max = -1U;
	size_t min_min = -1U;
	int max_len = 0;
	MergeTreeData::DataParts::iterator best_begin;
	bool found = false;

	DayNum_t now_day = date_lut.toDayNum(time(0));
	DayNum_t now_month = date_lut.toFirstDayNumOfMonth(now_day);
	int now_hour = date_lut.toHourInaccurate(time(0));

	/// Сколько кусков, начиная с текущего, можно включить в валидный отрезок, начинающийся левее текущего куска.
	/// Нужно для определения максимальности по включению.
	int max_count_from_left = 0;

	size_t cur_max_rows_to_merge_parts = data.settings.max_rows_to_merge_parts;

	/// Если ночь, можем мерджить сильно большие куски
	if (now_hour >= 1 && now_hour <= 5)
		cur_max_rows_to_merge_parts *= data.settings.merge_parts_at_night_inc;

	if (only_small)
	{
		cur_max_rows_to_merge_parts = data.settings.max_rows_to_merge_parts_second;
	}

	/// Левый конец отрезка.
	for (MergeTreeData::DataParts::iterator it = data_parts.begin(); it != data_parts.end(); ++it)
	{
		const MergeTreeData::DataPartPtr & first_part = *it;

		max_count_from_left = std::max(0, max_count_from_left - 1);

		/// Кусок достаточно мал или слияние "агрессивное".
		if (first_part->size * data.index_granularity > cur_max_rows_to_merge_parts
			&& !aggressive)
			continue;

		/// Кусок в одном месяце.
		if (first_part->left_month != first_part->right_month)
		{
			LOG_WARNING(log, "Part " << first_part->name << " spans more than one month");
			continue;
		}

		/// Самый длинный валидный отрезок, начинающийся здесь.
		size_t cur_longest_max = -1U;
		size_t cur_longest_min = -1U;
		int cur_longest_len = 0;

		/// Текущий отрезок, не обязательно валидный.
		size_t cur_max = first_part->size;
		size_t cur_min = first_part->size;
		size_t cur_sum = first_part->size;
		size_t cur_total_size = first_part->size_in_bytes;
		int cur_len = 1;

		DayNum_t month = first_part->left_month;
		UInt64 cur_id = first_part->right;

		/// Этот месяц кончился хотя бы день назад.
		bool is_old_month = now_day - now_month >= 1 && now_month > month;

		time_t oldest_modification_time = first_part->modification_time;

		/// Правый конец отрезка.
		MergeTreeData::DataParts::iterator jt = it;
		for (; cur_len < static_cast<int>(data.settings.max_parts_to_merge_at_once);)
		{
			const MergeTreeData::DataPartPtr & prev_part = *jt;
			++jt;

			if (jt == data_parts.end())
				break;

			const MergeTreeData::DataPartPtr & last_part = *jt;

			/// Кусок разрешено сливать с предыдущим, и в одном правильном месяце.
			if (!can_merge(prev_part, last_part) ||
				last_part->left_month != last_part->right_month ||
				last_part->left_month != month)
				break;

			/// Кусок достаточно мал или слияние "агрессивное".
			if (last_part->size * data.index_granularity > cur_max_rows_to_merge_parts
				&& !aggressive)
				break;

			/// Кусок правее предыдущего.
			if (last_part->left < cur_id)
			{
				LOG_WARNING(log, "Part " << last_part->name << " intersects previous part");
				break;
			}

			oldest_modification_time = std::max(oldest_modification_time, last_part->modification_time);
			cur_max = std::max(cur_max, last_part->size);
			cur_min = std::min(cur_min, last_part->size);
			cur_sum += last_part->size;
			cur_total_size += last_part->size_in_bytes;
			++cur_len;
			cur_id = last_part->right;

			int min_len = 2;
			int cur_age_in_sec = time(0) - oldest_modification_time;

			/// Если куски примерно больше 1 Gb и образовались меньше 6 часов назад, то мерджить не меньше чем по 3.
			if (cur_max * data.index_granularity * 150 > 1024*1024*1024 && cur_age_in_sec < 6*3600)
				min_len = 3;

			/// Равен 0.5 если возраст порядка 0, равен 5 если возраст около месяца.
			double time_ratio_modifier = 0.5 + 9 * static_cast<double>(cur_age_in_sec) / (3600*24*30 + cur_age_in_sec);

			/// Двоичный логарифм суммарного размера кусочков
			double log_cur_sum = std::log(cur_sum * data.index_granularity) / std::log(2);
			/// Равен ~2 если куски маленькие, уменьшается до 0.5 с увеличением суммарного размера до 2^25.
			double size_ratio_modifier = std::max(0.25, 2 - 3 * (log_cur_sum) / (25 + log_cur_sum));

			/// Объединяем все в одну константу
			double ratio = std::max(0.5, time_ratio_modifier * size_ratio_modifier * data.settings.max_size_ratio_to_merge_parts);

			/// Если отрезок валидный, то он самый длинный валидный, начинающийся тут.
			if (cur_len >= min_len
				&& (static_cast<double>(cur_max) / (cur_sum - cur_max) < ratio
					/// За старый месяц объединяем что угодно, если разрешено и если этому хотя бы 15 дней
					|| (is_old_month && merge_anything_for_old_months && cur_age_in_sec > 3600*24*15)
					/// Если слияние "агрессивное", то сливаем что угодно
					|| aggressive))
			{
				/// Достаточно места на диске, чтобы покрыть новый мердж с запасом.
				if (available_disk_space > cur_total_size * DISK_USAGE_COEFFICIENT_TO_SELECT)
				{
					cur_longest_max = cur_max;
					cur_longest_min = cur_min;
					cur_longest_len = cur_len;
				}
				else
					LOG_WARNING(log, "Won't merge parts from " << first_part->name << " to " << last_part->name
						<< " because not enough free space: " << available_disk_space << " free and unreserved, "
						<< cur_total_size << " required now (+" << static_cast<int>((DISK_USAGE_COEFFICIENT_TO_SELECT - 1.0) * 100)
						<< "% on overhead)");
			}
		}

		/// Это максимальный по включению валидный отрезок.
		if (cur_longest_len > max_count_from_left)
		{
			max_count_from_left = cur_longest_len;

			if (!found
				|| std::make_pair(std::make_pair(cur_longest_max, cur_longest_min), -cur_longest_len)
					< std::make_pair(std::make_pair(min_max, min_min), -max_len))
			{
				found = true;
				min_max = cur_longest_max;
				min_min = cur_longest_min;
				max_len = cur_longest_len;
				best_begin = it;
			}
		}
	}

	if (found)
	{
		parts.clear();

		MergeTreeData::DataParts::iterator it = best_begin;
		for (int i = 0; i < max_len; ++i)
		{
			parts.push_back(*it);
			++it;
		}

		LOG_DEBUG(log, "Selected " << parts.size() << " parts from " << parts.front()->name << " to " << parts.back()->name);
	}
	else
	{
		LOG_DEBUG(log, "No parts to merge");
	}

	return found;
}


/// parts должны быть отсортированы.
String MergeTreeDataMerger::mergeParts(const MergeTreeData::DataPartsVector & parts)
{
	LOG_DEBUG(log, "Merging " << parts.size() << " parts: from " << parts.front()->name << " to " << parts.back()->name);

	Names all_column_names;
	NamesAndTypesList columns_list = data.getColumnsList();
	for (const auto & it : columns_list)
		all_column_names.push_back(it.first);

	DateLUTSingleton & date_lut = DateLUTSingleton::instance();

	MergeTreeData::MutableDataPartPtr new_data_part = std::make_shared<MergeTreeData::DataPart>(data);
	new_data_part->left_date = std::numeric_limits<UInt16>::max();
	new_data_part->right_date = std::numeric_limits<UInt16>::min();
	new_data_part->left = parts.front()->left;
	new_data_part->right = parts.back()->right;
	new_data_part->level = 0;
	for (size_t i = 0; i < parts.size(); ++i)
	{
		new_data_part->level = std::max(new_data_part->level, parts[i]->level);
		new_data_part->left_date = std::min(new_data_part->left_date, parts[i]->left_date);
		new_data_part->right_date = std::max(new_data_part->right_date, parts[i]->right_date);
	}
	++new_data_part->level;
	new_data_part->name = MergeTreeData::getPartName(
		new_data_part->left_date, new_data_part->right_date, new_data_part->left, new_data_part->right, new_data_part->level);
	new_data_part->left_month = date_lut.toFirstDayNumOfMonth(new_data_part->left_date);
	new_data_part->right_month = date_lut.toFirstDayNumOfMonth(new_data_part->right_date);

	/** Читаем из всех кусков, сливаем и пишем в новый.
	  * Попутно вычисляем выражение для сортировки.
	  */
	BlockInputStreams src_streams;

	for (size_t i = 0; i < parts.size(); ++i)
	{
		MarkRanges ranges(1, MarkRange(0, parts[i]->size));
		src_streams.push_back(new ExpressionBlockInputStream(new MergeTreeBlockInputStream(
			data.getFullPath() + parts[i]->name + '/', DEFAULT_MERGE_BLOCK_SIZE, all_column_names, data,
			parts[i], ranges, false, NULL, ""), data.getPrimaryExpression()));
	}

	/// Порядок потоков важен: при совпадении ключа элементы идут в порядке номера потока-источника.
	/// В слитом куске строки с одинаковым ключом должны идти в порядке возрастания идентификатора исходного куска, то есть (примерного) возрастания времени вставки.
	BlockInputStreamPtr merged_stream;

	switch (data.mode)
	{
		case MergeTreeData::Ordinary:
			merged_stream = new MergingSortedBlockInputStream(src_streams, data.getSortDescription(), DEFAULT_MERGE_BLOCK_SIZE);
			break;

		case MergeTreeData::Collapsing:
			merged_stream = new CollapsingSortedBlockInputStream(src_streams, data.getSortDescription(), data.sign_column, DEFAULT_MERGE_BLOCK_SIZE);
			break;

		case MergeTreeData::Summing:
			merged_stream = new SummingSortedBlockInputStream(src_streams, data.getSortDescription(), DEFAULT_MERGE_BLOCK_SIZE);
			break;

		default:
			throw Exception("Unknown mode of operation for MergeTreeData: " + toString(data.mode), ErrorCodes::LOGICAL_ERROR);
	}

	MergedBlockOutputStreamPtr to = new MergedBlockOutputStream(data,
		new_data_part->left_date, new_data_part->right_date, new_data_part->left, new_data_part->right, new_data_part->level);

	merged_stream->readPrefix();
	to->writePrefix();

	Block block;
	while (!canceled && (block = merged_stream->read()))
		to->write(block);

	if (canceled)
	{
		LOG_INFO(log, "Canceled merging parts.");
		return "";
	}

	merged_stream->readSuffix();
	to->writeSuffix();

	/// В обычном режиме строчки не могут удалиться при мердже.
	if (0 == to->marksCount() && data.mode == MergeTreeData::Ordinary)
		throw Exception("Empty part after merge", ErrorCodes::LOGICAL_ERROR);

	new_data_part->size = to->marksCount();
	new_data_part->modification_time = time(0);

	if (0 == to->marksCount())
	{
		LOG_INFO(log, "All rows have been deleted while merging from " << parts.front()->name << " to " << parts.back()->name);
		return "";
	}

	/// NOTE Только что записанный индекс заново считывается с диска. Можно было бы формировать его сразу при записи.
	new_data_part->loadIndex();

	/// Добавляем новый кусок в набор.
	data.replaceParts(parts, new_data_part);

	LOG_TRACE(log, "Merged " << parts.size() << " parts: from " << parts.front()->name << " to " << parts.back()->name);

	return new_data_part->name;
}

size_t MergeTreeDataMerger::estimateDiskSpaceForMerge(const MergeTreeData::DataPartsVector & parts)
{
	size_t res = 0;
	for (const MergeTreeData::DataPartPtr & part : parts)
	{
		res += part->size_in_bytes;
	}
	return static_cast<size_t>(res * DISK_USAGE_COEFFICIENT_TO_RESERVE);
}

}