#pragma once

#include <DB/Storages/IStorage.h>
#include <DB/Parsers/ASTSelectQuery.h>
#include <DB/Storages/StorageView.h>


namespace DB
{

class StorageMaterializedView : public StorageView {

public:
	static StoragePtr create(const String & table_name_, const String & database_name_,
		Context & context_,	ASTPtr & query_, NamesAndTypesListPtr columns_, bool attach_);

	std::string getName() const { return "MaterializedView"; }
	std::string getInnerTableName() const { return  ".inner." + table_name; }

	BlockOutputStreamPtr write(ASTPtr query);
	void dropImpl();
	bool optimize();

	BlockInputStreams read(
		const Names & column_names,
		ASTPtr query,
		const Settings & settings,
		QueryProcessingStage::Enum & processed_stage,
		size_t max_block_size = DEFAULT_BLOCK_SIZE,
		unsigned threads = 1);


private:
	StoragePtr data;

	StorageMaterializedView(const String & table_name_, const String & database_name_,
		Context & context_,	ASTPtr & query_, NamesAndTypesListPtr columns_,	bool attach_);

};

}